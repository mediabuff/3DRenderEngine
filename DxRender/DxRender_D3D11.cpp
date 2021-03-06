#include "DxRender_D3D11.h"
#include "d3dAdapterOutputEnumerator.h"
#include <assert.h>
#include <fstream>
#include "YUVTexture_Packed.h"
#include "YUVTexture_Planar.h"
#include "ARGBTexture_8.h"
#include "YUVTexture_NV12.h"
#include <DxErr.h>
#include "DXLogger.h"
#include "libtext.h"
#include "SharedFrameTexture.h"
#include "inc/TextureResource.h"
#include "D3D11TextureRender.h"

using namespace zRender;

#define LOG_TAG L"DxRender_D3D11"

DxRender_D3D11::DxRender_D3D11()
	: m_adapter(NULL), m_device(NULL), m_context(NULL), m_swapChain(NULL)
	, m_hWnd(NULL), m_winWidth(0), m_winHeight(0), m_aspectRatio(0)
	, m_depthBuffer(NULL), m_depthView(NULL)
	, m_bkbufDxgiSurface(NULL)
//	, m_backgroundComponent(NULL)
	, m_TransparentBS(NULL)
	, m_bEnable4xMsaa(false)
	, m_renderTargetView(NULL)
	, m_renderTargetTexture(NULL)
	, m_color(0)
	, m_offscreenRttRender(NULL)
{
	XMMATRIX I = XMMatrixIdentity();
	XMStoreFloat4x4(&m_worldBaseTransform, I);
	XMStoreFloat4x4(&m_viewTransform, I);
	XMStoreFloat4x4(&m_projTransform, I);
}

DxRender_D3D11::~DxRender_D3D11()
{
//	cancleBackground();
	deinit();
}

int DxRender_D3D11::init(HWND hWnd, const wchar_t* effectFileName, bool isEnable4XMSAA /*= false*/, bool isSDICompatible/* = false*/)
{
	IDXGIAdapter* dstAdapter = NULL;
	if(S_OK!=DXGI_getNearestAdapter(hWnd, &dstAdapter))
	{
		log_e(LOG_TAG, L"DXGI get adapter from hWnd failed.");
		return -1;
	}

	DXGI_ADAPTER_DESC adptDesc;
	if(S_OK==dstAdapter->GetDesc(&adptDesc))
	{
		log_e(LOG_TAG, libtext::format(L"DXGI get adapter from hWnd success.Adapter name [%s]", adptDesc.Description));
	}
	UINT createDeviceFlags = 0;
//#if defined(DEBUG) || defined(_DEBUG)  
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
//#endif

	D3D_FEATURE_LEVEL curFeatureLv = D3D_FEATURE_LEVEL_9_1;
	D3D_FEATURE_LEVEL featureLvs[] = {
		//D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
		D3D_FEATURE_LEVEL_9_2,
		D3D_FEATURE_LEVEL_9_1
	};

	HRESULT hr = D3D11CreateDevice(
									dstAdapter,                 // use selected adapter
									D3D_DRIVER_TYPE_UNKNOWN,	//adapter is No NULL
									0,							// no software device
									createDeviceFlags, 
									featureLvs, ARRAYSIZE(featureLvs),// default feature level array
									D3D11_SDK_VERSION,
									&m_device,
									&curFeatureLv,
									&m_context);
	if(S_OK!=hr)
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : faile to Create device with param HWND(%u) Adapter(%d) CreateFlag(%d) ErrorCode=%d",
			(unsigned int)hWnd, (int)dstAdapter, createDeviceFlags, (int)hr);
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : faile to Create device with param Adapter(%d) CreateFlag(%d) ErrorCode=%d\n",
			(int)dstAdapter, createDeviceFlags, (int)hr);
#endif
		log_e(LOG_TAG, errmsg);
		return -2;
	}

	if( curFeatureLv < D3D_FEATURE_LEVEL_11_0 )
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : Direct3D Feature Level 11 unsupported.\n");
#endif
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : Direct3D Feature Level 11 unsupported. FeatureLevel=%d",
			(int)curFeatureLv);
		log_e(LOG_TAG, errmsg);
		return -3;
	}
	dstAdapter->Release();

	// Check 4X MSAA quality support for our back buffer format.
	// All Direct3D 11 capable devices support 4X MSAA for all render 
	// target formats, so we only need to check quality support.
	UINT i4xMsaaQuality = 0;
	if(S_OK!=m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 4, &i4xMsaaQuality))
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : CheckMultisampleQualityLevels failed.\n");
#endif
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : CheckMultisampleQualityLevels failed.");
		log_e(LOG_TAG, errmsg);
	}
	if( i4xMsaaQuality<=0 && isEnable4XMSAA)
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : check quality support failed\n");
#endif
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : Need 4X MSAA, But the device is Not Support.");
		log_e(LOG_TAG, errmsg);
		return -4;
	}

	RECT winRect = {0, 0, 0, 0};
	/*GetWindowRect*/GetClientRect(hWnd, &winRect);
	m_winWidth = winRect.right - winRect.left;
	m_winHeight = winRect.bottom - winRect.top;
	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferDesc.Width  = m_winWidth;
	sd.BufferDesc.Height = m_winHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount  = 1;
	sd.OutputWindow = hWnd;
	sd.Windowed     = true;
	sd.SwapEffect   = DXGI_SWAP_EFFECT_DISCARD;
	sd.Flags        =  0;
	if(isSDICompatible)
	{
		sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.Flags |= DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
	}
	// Use 4X MSAA? 
	if( isEnable4XMSAA )
	{
		sd.SampleDesc.Count   = 4;
		sd.SampleDesc.Quality = i4xMsaaQuality-1;
	}
	// No MSAA
	else
	{
		sd.SampleDesc.Count   = 1;
		sd.SampleDesc.Quality = 0;
	}

	// To correctly create the swap chain, we must use the IDXGIFactory that was
	// used to create the device.  If we tried to use a different IDXGIFactory instance
	// (by calling CreateDXGIFactory), we get an error: "IDXGIFactory::CreateSwapChain: 
	// This function is being called with a device from a different IDXGIFactory."
	IDXGIDevice1* dxgiDevice = 0;
	if(S_OK!=m_device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : QueryInterface IDXGIDevice failed.");
		log_e(LOG_TAG, errmsg);
		return -5;	      
	}
	IDXGIAdapter1* dxgiAdapter = 0;
	if(S_OK!=dxgiDevice->GetParent(__uuidof(IDXGIAdapter1), (void**)&dxgiAdapter))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : QueryInterface IDXGIAdapter failed.");
		log_e(LOG_TAG, errmsg);
		return -6;
	}
	IDXGIFactory1* dxgiFactory = 0;
	if(S_OK!=dxgiAdapter->GetParent(__uuidof(IDXGIFactory1), (void**)&dxgiFactory))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : QueryInterface IDXGIFactory failed.");
		log_e(LOG_TAG, errmsg);
		return -7;
	}
	m_hWnd = hWnd;

	HRESULT rslt = S_FALSE;
	if(S_OK!= (rslt=dxgiFactory->CreateSwapChain(m_device, &sd, &m_swapChain)))
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : Create Swap Chain failed.(Width=%d, Height=%d, SampleCount=%d, SampleQuality=%d)\n",
			sd.BufferDesc.Width, sd.BufferDesc.Height, sd.SampleDesc.Count, sd.SampleDesc.Quality);
#endif
		TCHAR errmsg[1024] = {0};
		swprintf_s(errmsg, 1024, L"Error in DxRender_D3D11::init : Create Swap Chain failed.ERR:%s (Width=%d, Height=%d, Format=%d Flags=%d SampleCount=%d, SampleQuality=%d)\n",
			DXGetErrorDescription(rslt), sd.BufferDesc.Width, sd.BufferDesc.Height, sd.BufferDesc.Format, sd.Flags, sd.SampleDesc.Count, sd.SampleDesc.Quality);
		log_e(LOG_TAG, errmsg);
		return -8;
	}

	ReleaseCOM(dxgiDevice);
	ReleaseCOM(dxgiAdapter);
	ReleaseCOM(dxgiFactory);

	ID3D11Texture2D* backBuffer;
	if(S_OK!=m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))
		return -9;
	if(S_OK!=m_device->CreateRenderTargetView(backBuffer, 0, &m_renderTargetView))
	{
		ReleaseCOM(backBuffer);
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : CreateRenderTargetView failed.");
		log_e(LOG_TAG, errmsg);
		return -10;
	}
	ReleaseCOM(backBuffer);

	// Create the depth/stencil buffer and view.

	D3D11_TEXTURE2D_DESC depthStencilDesc;
	
	depthStencilDesc.Width     = m_winWidth;
	depthStencilDesc.Height    = m_winHeight;;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.ArraySize = 1;
	depthStencilDesc.Format    = DXGI_FORMAT_D24_UNORM_S8_UINT;

	// Use 4X MSAA? --must match swap chain MSAA values.
	if( isEnable4XMSAA )
	{
		depthStencilDesc.SampleDesc.Count   = 4;
		depthStencilDesc.SampleDesc.Quality = i4xMsaaQuality-1;
	}
	// No MSAA
	else
	{
		depthStencilDesc.SampleDesc.Count   = 1;
		depthStencilDesc.SampleDesc.Quality = 0;
	}
	m_bEnable4xMsaa = isEnable4XMSAA;

	depthStencilDesc.Usage          = D3D11_USAGE_DEFAULT;
	depthStencilDesc.BindFlags      = D3D11_BIND_DEPTH_STENCIL;
	depthStencilDesc.CPUAccessFlags = 0; 
	depthStencilDesc.MiscFlags      = 0;

	if(S_OK!=m_device->CreateTexture2D(&depthStencilDesc, 0, &m_depthBuffer))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : create depth buffer failed.");
		log_e(LOG_TAG, errmsg);
		return -11;
	}
	if(S_OK!=m_device->CreateDepthStencilView(m_depthBuffer, 0, &m_depthView))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : create depth stencil view failed.");
		log_e(LOG_TAG, errmsg);
		return -12;
	}

	// Bind the render target view and depth/stencil view to the pipeline.

	m_context->OMSetRenderTargets(1, &m_renderTargetView, m_depthView);
	

	// Set the viewport transform.

	m_viewport.TopLeftX = 0;
	m_viewport.TopLeftY = 0;
	m_viewport.Width    = static_cast<float>(m_winWidth);
	m_viewport.Height   = static_cast<float>(m_winHeight);
	m_viewport.MinDepth = 0.0f;
	m_viewport.MaxDepth = 1.0f;

	m_context->RSSetViewports(1, &m_viewport);

	m_material.Ambient  = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f);
	m_material.Diffuse  = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	m_material.Specular = XMFLOAT4(0.6f, 0.6f, 0.6f, 16.0f);

	//create transparent blend state
	D3D11_BLEND_DESC transparentDesc = {0};
	transparentDesc.AlphaToCoverageEnable = false;
	transparentDesc.IndependentBlendEnable = false;
	transparentDesc.RenderTarget[0].BlendEnable = true;
	transparentDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	transparentDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	transparentDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	transparentDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	transparentDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	transparentDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	transparentDesc.RenderTarget[0].RenderTargetWriteMask =	D3D11_COLOR_WRITE_ENABLE_ALL;
	if(FAILED(m_device->CreateBlendState(&transparentDesc, &m_TransparentBS)))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : create alpha blend state failed.");
		log_e(LOG_TAG, errmsg);
		return -14;
	}
	log_e(LOG_TAG, L"init success.");
	return 0;
}

int zRender::DxRender_D3D11::init(HMONITOR hmonitor)
{
	IDXGIAdapter* dstAdapter = NULL;
	if(S_OK!=DXGI_getAdapter(hmonitor, &dstAdapter))
	{
		log_e(LOG_TAG, L"DXGI get adapter from hWnd failed.");
		return -1;
	}

	DXGI_ADAPTER_DESC adptDesc;
	if(S_OK==dstAdapter->GetDesc(&adptDesc))
	{
		log_e(LOG_TAG, libtext::format(L"DXGI get adapter from hWnd success.Adapter name [%s]", adptDesc.Description));
	}
	UINT createDeviceFlags = 0;
#if defined(DEBUG) || defined(_DEBUG)  
	createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL curFeatureLv = D3D_FEATURE_LEVEL_9_1;
	D3D_FEATURE_LEVEL featureLvs[] = {
		//D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
		D3D_FEATURE_LEVEL_9_2,
		D3D_FEATURE_LEVEL_9_1
	};

	HRESULT hr = D3D11CreateDevice(
		dstAdapter,                 // use selected adapter
		D3D_DRIVER_TYPE_UNKNOWN,	//adapter is No NULL
		0,							// no software device
		createDeviceFlags, 
		featureLvs, ARRAYSIZE(featureLvs),// default feature level array
		D3D11_SDK_VERSION,
		&m_device,
		&curFeatureLv,
		&m_context);
	if(S_OK!=hr)
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : faile to Create device with param HMONITOR(%u) Adapter(%d) CreateFlag(%d) ErrorCode=%d",
			(unsigned int)hmonitor, (int)dstAdapter, createDeviceFlags, (int)hr);
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : faile to Create device with param Adapter(%d) CreateFlag(%d) ErrorCode=%d\n",
			(int)dstAdapter, createDeviceFlags, (int)hr);
#endif
		log_e(LOG_TAG, errmsg);
		return -2;
	}
	dstAdapter->Release();

	if( curFeatureLv < D3D_FEATURE_LEVEL_11_0 )
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : Direct3D Feature Level 11 unsupported.\n");
#endif
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : Direct3D Feature Level 11 unsupported. FeatureLevel=%d",
			(int)curFeatureLv);
		log_e(LOG_TAG, errmsg);
		return -3;
	}

	return 0;
}

int zRender::DxRender_D3D11::init( int width, int height, int adapter, const wchar_t* effectFileName, bool isEnable4XMSAA /*= false*/, bool isSDICompatible /*= false*/ )
{
	//判断参数是否有效
	std::vector<IDXGIAdapter*> adapterVec;
	if(S_OK!=DXGI_getAdapters(adapterVec))
	{
		log_e(LOG_TAG, L"DXGI get adapter list failed.");
		return -1;
	}
	if(adapter>=adapterVec.size())
	{
		log_e(LOG_TAG, L"Param invalid. adapter is too big");
		DXGI_releaseAdaptersObjs(adapterVec);
		return -2;
	}

	if(width<=0 || height<=0)
	{
		log_e(LOG_TAG, L"Param invalid. width height");
		DXGI_releaseAdaptersObjs(adapterVec);
		return -3;
	}
	UINT createDeviceFlags = 0;
	//#if defined(DEBUG) || defined(_DEBUG)  
	createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
	//#endif

	D3D_FEATURE_LEVEL curFeatureLv = D3D_FEATURE_LEVEL_9_1;
	D3D_FEATURE_LEVEL featureLvs[] = {
		//D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
		D3D_FEATURE_LEVEL_9_2,
		D3D_FEATURE_LEVEL_9_1
	};

	HRESULT hr = D3D11CreateDevice(
		adapterVec[adapter],                 // use selected adapter
		D3D_DRIVER_TYPE_UNKNOWN,	//adapter is No NULL
		0,							// no software device
		createDeviceFlags, 
		featureLvs, ARRAYSIZE(featureLvs),// default feature level array
		D3D11_SDK_VERSION,
		&m_device,
		&curFeatureLv,
		&m_context);
	if(S_OK!=hr)
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : faile to Create device with param Adapter(%d) CreateFlag(%d) ErrorCode=%d",
			(int)adapter, createDeviceFlags, (int)hr);
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : faile to Create device with param Adapter(%d) CreateFlag(%d) ErrorCode=%d\n",
			(int)dstAdapter, createDeviceFlags, (int)hr);
#endif
		log_e(LOG_TAG, errmsg);
		DXGI_releaseAdaptersObjs(adapterVec);
		return -4;
	}
	DXGI_releaseAdaptersObjs(adapterVec);

	if( curFeatureLv < D3D_FEATURE_LEVEL_11_0 )
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : Direct3D Feature Level 11 unsupported.\n");
#endif
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : Direct3D Feature Level 11 unsupported. FeatureLevel=%d",
			(int)curFeatureLv);
		log_e(LOG_TAG, errmsg);
		return -5;
	}

	// Check 4X MSAA quality support for our back buffer format.
	// All Direct3D 11 capable devices support 4X MSAA for all render 
	// target formats, so we only need to check quality support.
	UINT i4xMsaaQuality = 0;
	if(S_OK!=m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 4, &i4xMsaaQuality))
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : CheckMultisampleQualityLevels failed.\n");
#endif
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : CheckMultisampleQualityLevels failed.");
		log_e(LOG_TAG, errmsg);
	}
	if( i4xMsaaQuality<=0 && isEnable4XMSAA)
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : check quality support failed\n");
#endif
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : Need 4X MSAA, But the device is Not Support.");
		log_e(LOG_TAG, errmsg);
		return -6;
	}

	//创建RenderTarget buffer与Render Target View
	D3D11_TEXTURE2D_DESC textureDesc;
	HRESULT result;
	D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc;
	D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc;

	// Initialize the render target texture description.
	ZeroMemory(&textureDesc, sizeof(textureDesc));

	// Setup the render target texture description.
	textureDesc.Width = width;
	textureDesc.Height = height;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	textureDesc.CPUAccessFlags = 0;
	textureDesc.MiscFlags = 0;

	// Create the render target texture.
	result = m_device->CreateTexture2D(&textureDesc, NULL, &m_renderTargetBuffer);
	if(FAILED(result))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : create Texture2D for render target failed.");
		return -7;
	}

	// Setup the description of the render target view.
	renderTargetViewDesc.Format = textureDesc.Format;
	renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	renderTargetViewDesc.Texture2D.MipSlice = 0;

	// Create the render target view.
	result = m_device->CreateRenderTargetView(m_renderTargetBuffer, &renderTargetViewDesc, &m_renderTargetView);
	if(FAILED(result))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : create Render Target view for offscreen Texture2D failed.");
		return -8;
	}

// 	// Setup the description of the shader resource view.
// 	shaderResourceViewDesc.Format = textureDesc.Format;
// 	shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
// 	shaderResourceViewDesc.Texture2D.MostDetailedMip = 0;
// 	shaderResourceViewDesc.Texture2D.MipLevels = 1;
// 
// 	// Create the shader resource view.
// 	result = m_device->CreateShaderResourceView(m_renderTargetBuffer, &shaderResourceViewDesc, &m_shaderResourceView);
// 	if(FAILED(result))
// 	{
// 		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : create Render Target view for offscreen Texture2D failed.");
// 		return false;
// 	}
//	m_width = textureWidth;
//	m_height = textureHeight;

	// Create the depth/stencil buffer and view.

	D3D11_TEXTURE2D_DESC depthStencilDesc;

	depthStencilDesc.Width = width;
	depthStencilDesc.Height = height;;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.ArraySize = 1;
	depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

	// Use 4X MSAA? --must match swap chain MSAA values.
	//if (isEnable4XMSAA)
	//{
	//	depthStencilDesc.SampleDesc.Count = 4;
	//	depthStencilDesc.SampleDesc.Quality = i4xMsaaQuality - 1;
	//}
	// No MSAA
	//else
	{
		depthStencilDesc.SampleDesc.Count = 1;
		depthStencilDesc.SampleDesc.Quality = 0;
	}
	//m_bEnable4xMsaa = isEnable4XMSAA;

	depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
	depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	depthStencilDesc.CPUAccessFlags = 0;
	depthStencilDesc.MiscFlags = 0;

	if (S_OK != m_device->CreateTexture2D(&depthStencilDesc, 0, &m_depthBuffer))
	{
		TCHAR errmsg[512] = { 0 };
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : OffscreenRTT create depth buffer failed.");
		log_e(LOG_TAG, errmsg);
		return -9;
	}
	if (S_OK != m_device->CreateDepthStencilView(m_depthBuffer, 0, &m_depthView))
	{
		TCHAR errmsg[512] = { 0 };
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : OffscreenRTT create depth stencil view failed.");
		log_e(LOG_TAG, errmsg);
		return -10;
	}
/*	在这种场景下SWAP CHAIN可能不再需要
	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferDesc.Width  = m_winWidth;
	sd.BufferDesc.Height = m_winHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount  = 1;
	sd.OutputWindow = hWnd;
	sd.Windowed     = true;
	sd.SwapEffect   = DXGI_SWAP_EFFECT_DISCARD;
	sd.Flags        =  0;
	if(isSDICompatible)
	{
		sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.Flags |= DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
	}
	// Use 4X MSAA? 
	if( isEnable4XMSAA )
	{
		sd.SampleDesc.Count   = 4;
		sd.SampleDesc.Quality = i4xMsaaQuality-1;
	}
	// No MSAA
	else
	{
		sd.SampleDesc.Count   = 1;
		sd.SampleDesc.Quality = 0;
	}

	// To correctly create the swap chain, we must use the IDXGIFactory that was
	// used to create the device.  If we tried to use a different IDXGIFactory instance
	// (by calling CreateDXGIFactory), we get an error: "IDXGIFactory::CreateSwapChain: 
	// This function is being called with a device from a different IDXGIFactory."
	IDXGIDevice1* dxgiDevice = 0;
	if(S_OK!=m_device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : QueryInterface IDXGIDevice failed.");
		log_e(LOG_TAG, errmsg);
		return -5;	      
	}
	IDXGIAdapter1* dxgiAdapter = 0;
	if(S_OK!=dxgiDevice->GetParent(__uuidof(IDXGIAdapter1), (void**)&dxgiAdapter))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : QueryInterface IDXGIAdapter failed.");
		log_e(LOG_TAG, errmsg);
		return -6;
	}
	IDXGIFactory1* dxgiFactory = 0;
	if(S_OK!=dxgiAdapter->GetParent(__uuidof(IDXGIFactory1), (void**)&dxgiFactory))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : QueryInterface IDXGIFactory failed.");
		log_e(LOG_TAG, errmsg);
		return -7;
	}
	m_hWnd = hWnd;

	HRESULT rslt = S_FALSE;
	if(S_OK!= (rslt=dxgiFactory->CreateSwapChain(m_device, &sd, &m_swapChain)))
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : Create Swap Chain failed.(Width=%d, Height=%d, SampleCount=%d, SampleQuality=%d)\n",
			sd.BufferDesc.Width, sd.BufferDesc.Height, sd.SampleDesc.Count, sd.SampleDesc.Quality);
#endif
		TCHAR errmsg[1024] = {0};
		swprintf_s(errmsg, 1024, L"Error in DxRender_D3D11::init : Create Swap Chain failed.ERR:%s (Width=%d, Height=%d, Format=%d Flags=%d SampleCount=%d, SampleQuality=%d)\n",
			DXGetErrorDescription(rslt), sd.BufferDesc.Width, sd.BufferDesc.Height, sd.BufferDesc.Format, sd.Flags, sd.SampleDesc.Count, sd.SampleDesc.Quality);
		log_e(LOG_TAG, errmsg);
		return -8;
	}

	//	rslt = m_swapChain->ResizeBuffers(1, m_winWidth, m_winHeight, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
	// 	if(FAILED(rslt))
	// 	{
	// 		printf("Failed to CreateSwapChain %s", DXGetErrorDescriptionA(rslt));
	// 		return -8;
	// 	}
	ReleaseCOM(dxgiDevice);
	ReleaseCOM(dxgiAdapter);
	ReleaseCOM(dxgiFactory);

	ID3D11Texture2D* backBuffer;
	if(S_OK!=m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))
		return -9;
	if(S_OK!=m_device->CreateRenderTargetView(m_renderTargetBuffer, 0, &m_renderTargetView))
	{
		ReleaseCOM(backBuffer);
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : CreateRenderTargetView failed.");
		log_e(LOG_TAG, errmsg);
		return -10;
	}
	ReleaseCOM(backBuffer);

	// Create the depth/stencil buffer and view.

	D3D11_TEXTURE2D_DESC depthStencilDesc;

	depthStencilDesc.Width     = m_winWidth;
	depthStencilDesc.Height    = m_winHeight;;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.ArraySize = 1;
	depthStencilDesc.Format    = DXGI_FORMAT_D24_UNORM_S8_UINT;

	// Use 4X MSAA? --must match swap chain MSAA values.
	if( isEnable4XMSAA )
	{
		depthStencilDesc.SampleDesc.Count   = 4;
		depthStencilDesc.SampleDesc.Quality = i4xMsaaQuality-1;
	}
	// No MSAA
	else
	{
		depthStencilDesc.SampleDesc.Count   = 1;
		depthStencilDesc.SampleDesc.Quality = 0;
	}
	m_bEnable4xMsaa = isEnable4XMSAA;

	depthStencilDesc.Usage          = D3D11_USAGE_DEFAULT;
	depthStencilDesc.BindFlags      = D3D11_BIND_DEPTH_STENCIL;
	depthStencilDesc.CPUAccessFlags = 0; 
	depthStencilDesc.MiscFlags      = 0;

	if(S_OK!=m_device->CreateTexture2D(&depthStencilDesc, 0, &m_depthBuffer))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : create depth buffer failed.");
		log_e(LOG_TAG, errmsg);
		return -11;
	}
	if(S_OK!=m_device->CreateDepthStencilView(m_depthBuffer, 0, &m_depthView))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : create depth stencil view failed.");
		log_e(LOG_TAG, errmsg);
		return -12;
	}
*/
	// Bind the render target view and depth/stencil view to the pipeline.

	m_context->OMSetRenderTargets(1, &m_renderTargetView, m_depthView);


	// Set the viewport transform.

	m_viewport.TopLeftX = 0;
	m_viewport.TopLeftY = 0;
	m_viewport.Width    = static_cast<float>(width);
	m_viewport.Height   = static_cast<float>(height);
	m_viewport.MinDepth = 0.0f;
	m_viewport.MaxDepth = 1.0f;

	m_context->RSSetViewports(1, &m_viewport);

	m_winWidth = width;
	m_winHeight = height;

	m_material.Ambient  = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f);
	m_material.Diffuse  = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	m_material.Specular = XMFLOAT4(0.6f, 0.6f, 0.6f, 16.0f);

	if(0!=createComputeShader(effectFileName))
	{
		TCHAR errmsg[1024] = {0};
		swprintf_s(errmsg, 1024, L"Error in DxRender_D3D11::init : Create compute shader from file [%s] failed", effectFileName);
		log_e(LOG_TAG, errmsg);
		return -13;
	}
	initEffectPass();
	createInputLayout();

	//create transparent blend state
	D3D11_BLEND_DESC transparentDesc = {0};
	transparentDesc.AlphaToCoverageEnable = false;
	transparentDesc.IndependentBlendEnable = false;
	transparentDesc.RenderTarget[0].BlendEnable = true;
	transparentDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	transparentDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	transparentDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	transparentDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	transparentDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	transparentDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	transparentDesc.RenderTarget[0].RenderTargetWriteMask =	D3D11_COLOR_WRITE_ENABLE_ALL;
	if(FAILED(m_device->CreateBlendState(&transparentDesc, &m_TransparentBS)))
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::init : create alpha blend state failed.");
		log_e(LOG_TAG, errmsg);
		return -14;
	}
	//	float blendFactors[] = {0.0f, 0.0f, 0.0f, 0.0f};
	//	m_context->OMSetBlendState(	m_TransparentBS, blendFactors, 0xffffffff);
	log_e(LOG_TAG, L"init success.");
	return 0;
}

void DxRender_D3D11::deinit()
{
	releaseOffscreenRenderTarget();
	ReleaseCOM(m_TransparentBS);
	ReleaseCOM(m_renderTargetView);
	ReleaseCOM(m_depthView);
	ReleaseCOM(m_depthBuffer);
	ReleaseCOM(m_swapChain);
	// Restore all default settings.
	if( m_context )
		m_context->ClearState();
	ReleaseCOM(m_context);
	ReleaseCOM(m_device);
}

int DxRender_D3D11::setVisibleRegion(const RECT_f& visibleReg)
{
	if(visibleReg.width()<=0 || visibleReg.height()<=0
		|| visibleReg.left<0 || visibleReg.top<0)
		return -1;

	//根据窗口的宽高比设置从物体坐标系到world坐标系的基本转换矩阵
	float aspectRatio = m_winWidth / (float)m_winHeight;
	XMMATRIX I = XMMatrixIdentity();
	XMStoreFloat4x4(&m_worldBaseTransform, I);
	m_worldBaseTransform._11 = aspectRatio;

	//设置生成从world坐标系到人眼坐标系的转换矩阵
	//XMVECTOR pos = XMVectorSet((visibleReg.left+visibleReg.right)*aspectRatio / 2, (visibleReg.top+visibleReg.bottom) / 2, -5, 1.0f);
	XMVECTOR pos = XMVectorSet(visibleReg.left*aspectRatio+visibleReg.width()*aspectRatio/2, -1.0f*(visibleReg.top+visibleReg.bottom) / 2, POS_ZINDEX_OF_EYE, 1.0f);
	//XMVECTOR target = XMVectorSet((visibleReg.left+visibleReg.right)*aspectRatio / 2, (visibleReg.top+visibleReg.bottom) / 2, 0, 1.0f);//XMVectorZero();
	XMVECTOR target = XMVectorSet(visibleReg.left*aspectRatio+visibleReg.width()*aspectRatio/2, -1.0f*(visibleReg.top+visibleReg.bottom) / 2, 0, 1.0f);//XMVectorZero();
	XMVECTOR up     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMMATRIX V = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&m_viewTransform, V);

	//生成Projection的转换矩阵
	XMMATRIX P = XMMatrixOrthographicLH(visibleReg.width()*aspectRatio, visibleReg.height(), RANGE_OF_ZINDEX_MIN, RANGE_OF_ZINDEX_MAX);
	//XMMATRIX P = XMMatrixOrthographicLH(1, 1, 1.0f, 1000.0f);
	XMStoreFloat4x4(&m_projTransform, P);

	m_visibleReg = visibleReg;
	m_aspectRatio = aspectRatio;
	return 0;
}

RECT_f DxRender_D3D11::getVisibleREgion() const
{
	return m_visibleReg;
}

DisplayElement* DxRender_D3D11::createDisplayElement(const RECT_f& displayReg, int zIndex)
{
	if(m_device==NULL || m_depthView==NULL)
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 Object have not be inited yet.\n");
		assert(false);
#endif
		return NULL;
	}

	if(!displayReg.isIntersect(m_visibleReg))
	{
#ifdef _DEBUG
		printf("Failed To Create DisplayElement[L-%f, T-%f, R-%f, B-%f], out of DxRender Visible region[L-%f, T-%f, R-%f, B-%f].\n",
			displayReg.left, displayReg.top, displayReg.right, displayReg.bottom,
			m_visibleReg.left, m_visibleReg.top, m_visibleReg.right, m_visibleReg.bottom);
#endif
		TCHAR errmsg[1024] = {0};
		swprintf_s(errmsg, 1024, L"Failed To Create DisplayElement[L-%f, T-%f, R-%f, B-%f], out of DxRender Visible region[L-%f, T-%f, R-%f, B-%f].\n",
				displayReg.left, displayReg.top, displayReg.right, displayReg.bottom,
				m_visibleReg.left, m_visibleReg.top, m_visibleReg.right, m_visibleReg.bottom);
		log_e(LOG_TAG, errmsg);
		return NULL;
	}

	DisplayElement* de = new DisplayElement(this, m_device, m_context);
	assert(de);
	int ret = de->setDisplayRegion(displayReg, (float)zIndex);
	assert(0==ret);
	if(0!=ret)
	{
		TCHAR errmsg[512] = {0};
		swprintf_s(errmsg, 512, L"Failed to setDisplayRegion of display elem.");
		log_e(LOG_TAG, errmsg);
	}
	return de;
}

int DxRender_D3D11::releaseDisplayElement(DisplayElement** displayElement)
{
	if(NULL==displayElement)
		return -1;
	if(*displayElement!=NULL)
	{
		DisplayElement* pelem = *displayElement;
		int ret = pelem->releaseRenderResource();
		if(0!=ret)
			return ret;
		//delete *displayElement;
		delete pelem;
		*displayElement = NULL;
	}
	return 0;
}

IRawFrameTexture* DxRender_D3D11::createTexture(PIXFormat pixFmt, int width, int height, 
					unsigned char* initData/* = NULL*/, int initDataLen /*= 0*/, bool isShared /*= false*/)
{
	if(m_device==NULL || m_depthView==NULL)
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 Object have not be inited yet.\n");
		assert(false);
#endif
		return NULL;
	}

	if(pixFmt==PIXFMT_UNKNOW || width<=0 || height<=0 
		|| (initData!=NULL && initDataLen<=0) || (initData==NULL &&initDataLen>0))
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 failed to create Texture.Param invalid.[Fmt=%d W=%d H=%d data=%d dataLen=%d]\n",
			pixFmt, width, height, (int)initData, initDataLen);
		assert(false);
#endif
		TCHAR errmsg[1024] = {0};
		swprintf_s(errmsg, 1024, L"DxRender_D3D11 failed to create Texture.Param invalid.[Fmt=%d W=%d H=%d data=%d dataLen=%d]",
				pixFmt, width, height, (int)initData, initDataLen);
		log_e(LOG_TAG, errmsg);
		return NULL;
	}

	//YUVTexture_Packed* tex = new YUVTexture_Packed(pixFmt);
	IRawFrameTexture* tex = NULL;
	switch(pixFmt)
	{
	case PIXFMT_A8R8G8B8:
	case PIXFMT_R8G8B8:
	case PIXFMT_R8G8B8A8:
	case PIXFMT_B8G8R8A8:
	case PIXFMT_B8G8R8X8:
	case PIXFMT_X8R8G8B8:
		tex = new ARGBTexture_8(pixFmt);
		break;
	case PIXFMT_YUY2:
		tex = new YUVTexture_Packed(pixFmt);
		break;
	case PIXFMT_YUV420P:
	case PIXFMT_YV12:
		tex = new YUVTexture_Planar(pixFmt);
		break;
	case PIXFMT_NV12:
		tex = new YUVTexture_NV12(pixFmt);
		break;
	default:
		{
			TCHAR errmsg[1024] = {0};
			swprintf_s(errmsg, 1024, L"DxRender_D3D11 failed to create Texture.unsurport Pixfmt.[Fmt=%d]", pixFmt);
			log_e(LOG_TAG, errmsg);
		}
		break;
	}
	if(NULL==tex || 0!=tex->create(m_device, width, height, (char*)initData, initDataLen))
	{
		delete tex;
		TCHAR errmsg[1024] = {0};
		swprintf_s(errmsg, 1024, L"tex->create failed");
		log_e(LOG_TAG, errmsg);
		return NULL;
	}

	return tex;
}

IRawFrameTexture * DxRender_D3D11::createTexture(PIXFormat pixfmt, int width, int height, TEXTURE_USAGE usage, bool bShared, unsigned char * initData, int dataLen, int pitch)
{
	if (m_device == NULL)
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 Object have not be inited yet.\n");
		assert(false);
#endif
		return NULL;
	}
	IRawFrameTexture* tex = NULL;
	switch (pixfmt)
	{
	case PIXFMT_A8R8G8B8:
	case PIXFMT_R8G8B8:
	case PIXFMT_R8G8B8A8:
	case PIXFMT_B8G8R8A8:
	case PIXFMT_B8G8R8X8:
	case PIXFMT_X8R8G8B8:
		tex = new ARGBTexture_8(pixfmt);
		break;
	case PIXFMT_YUY2:
		tex = new YUVTexture_Packed(pixfmt);
		break;
	case PIXFMT_YUV420P:
	case PIXFMT_YV12:
		tex = new YUVTexture_Planar(pixfmt);
		break;
	case PIXFMT_NV12:
		tex = new YUVTexture_NV12(pixfmt);
		break;
	default:
	{
		TCHAR errmsg[1024] = { 0 };
		swprintf_s(errmsg, 1024, L"DxRender_D3D11 failed to create Texture.unsurport Pixfmt.[Fmt=%d]", pixfmt);
		log_e(LOG_TAG, errmsg);
	}
	break;
	}
	if (NULL == tex || 0 != tex->create(m_device, width, height, usage, bShared, (char*)initData, dataLen, pitch))
	{
		delete tex;
		TCHAR errmsg[1024] = { 0 };
		swprintf_s(errmsg, 1024, L"tex->create failed");
		log_e(LOG_TAG, errmsg);
		return NULL;
	}

	return tex;
}

IRawFrameTexture * zRender::DxRender_D3D11::openSharedTexture(IRawFrameTexture * sharedTexture)
{
	if (sharedTexture == NULL)
		return NULL;
	PIXFormat pixfmt = sharedTexture->getPixelFormat();
	int width = sharedTexture->getWidth();
	int height = sharedTexture->getHeight();
	IRawFrameTexture* tex = NULL;
	switch (pixfmt)
	{
	case PIXFMT_A8R8G8B8:
	case PIXFMT_R8G8B8:
	case PIXFMT_R8G8B8A8:
	case PIXFMT_B8G8R8A8:
	case PIXFMT_B8G8R8X8:
	case PIXFMT_X8R8G8B8:
		tex = new ARGBTexture_8(pixfmt);
		break;
	case PIXFMT_YUY2:
		tex = new YUVTexture_Packed(pixfmt);
		break;
	case PIXFMT_YUV420P:
	case PIXFMT_YV12:
		tex = new YUVTexture_Planar(pixfmt);
		break;
	case PIXFMT_NV12:
		tex = new YUVTexture_NV12(pixfmt);
		break;
	default:
	{
		TCHAR errmsg[1024] = { 0 };
		swprintf_s(errmsg, 1024, L"DxRender_D3D11 failed to create Texture.unsurport Pixfmt.[Fmt=%d]", pixfmt);
		log_e(LOG_TAG, errmsg);
	}
	break;
	}
	if (NULL == tex || 0 != tex->openSharedTexture(m_device, sharedTexture))
	{
		delete tex;
		TCHAR errmsg[1024] = { 0 };
		swprintf_s(errmsg, 1024, L"tex->openSharedTexture failed");
		log_e(LOG_TAG, errmsg);
		return NULL;
	}
	return tex;
}

int DxRender_D3D11::releaseTexture(IRawFrameTexture** texture)
{
	if(NULL==texture)
		return -1;
	if(*texture!=NULL)
	{
		int ret = (*texture)->destroy();
		if(0!=ret)
			return ret;
		delete *texture;
		*texture = NULL;
	}
	return 0;
}

ID3D11Buffer* DxRender_D3D11::createBuffer(int byteCount, const unsigned char* initData, int initDataLen,
									D3D11_USAGE usage, UINT bindFlag)
{
	if(m_device==NULL || m_depthView==NULL)
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 Object have not be inited yet.\n");
		assert(false);
#endif
		return NULL;
	}
	if(byteCount<16 || initData==NULL || initDataLen<byteCount)
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::createVertexBuffer : param invalid.(BC=%d, data=%d, dataLen=%d)\n",
			byteCount, (int)initData, initDataLen);
#endif
		return NULL;
	}
	ID3D11Buffer* vtBuf = NULL;
	D3D11_BUFFER_DESC vbd;
    vbd.Usage = usage;
    vbd.ByteWidth = byteCount;
    vbd.BindFlags = bindFlag;
    vbd.CPUAccessFlags = 0;
    vbd.MiscFlags = 0;
    D3D11_SUBRESOURCE_DATA vinitData;
    vinitData.pSysMem = initData;
    if(S_OK!=m_device->CreateBuffer(&vbd, &vinitData, &vtBuf))
	{
		return NULL;
	}
	return vtBuf;
}

ID3D11Buffer* DxRender_D3D11::createVertexBuffer(int byteCount, const unsigned char* initData, int initDataLen)
{
	return createBuffer(byteCount, initData, initDataLen, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER);
}

ID3D11Buffer* DxRender_D3D11::createIndexBuffer(int byteCount, const unsigned char* initData, int initDataLen)
{
	return createBuffer(byteCount, initData, initDataLen, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER);
}

int DxRender_D3D11::releaseBuffer(ID3D11Buffer** buffer)
{
	if(buffer==NULL)	return -1;
	if(*buffer)
	{
		(*buffer)->Release();
		*buffer = NULL;
	}
	return 0;
}

int DxRender_D3D11::draw(DisplayElement* displayElem)
{
	if (m_drawCount == 0 && m_renderTargetTexture)
	{
		setRenderTargetTexture();
	}
	else if (m_renderTargetTexture)
	{
		OutputDebugStringA("---------\n");
	}
	m_drawCount++;
	if(m_device==NULL || m_depthView==NULL)
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 Object have not be inited yet.\n");
		assert(false);
#endif
		return -1;
	}
	if(displayElem==NULL)
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::draw : param displayElem is NULL.\n");
#endif
		return -2;
	}
	if (displayElem->getParentDxRender() != this)
	{
		log_e(LOG_TAG, L"Error in DxRender_D3D11::draw : the display elemement is not created by this DxRender object.");
		return -3;
	}
	return displayElem->draw();
}

int DxRender_D3D11::present(int type)
{
	if(!m_swapChain)	return -1;
	if (m_renderTargetTexture)
	{
		setRenderTargetBackbuffer();
		clearBackbuffer(m_color);
		drawOffscreenRenderTarget();
	}
	m_swapChain->Present(type, 0);
	//getSnapshot(NULL);
	return 0;
}

int DxRender_D3D11::clear(DWORD color)
{
	m_color = color;
	if (m_renderTargetTexture)
	{
		int a = (color & 0xff000000) >> 24;
		int r = (color & 0x00ff0000) >> 16;
		int g = (color & 0x0000ff00) >> 8;
		int b = (color & 0x000000ff) >> 0;
		m_renderTargetTexture->ClearRenderTarget(m_context, m_depthView, (float)r / 256, (float)g / 256, (float)b / 256, (float)a / 256);
		return 0;
	}
	else
	{
		return clearBackbuffer(color);
	}
}

int zRender::DxRender_D3D11::lockBackbufferHDC( BOOL Discard, HDC* outHDC )
{
	if(m_device==NULL || m_depthView==NULL)
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 Object have not be inited yet.\n");
		assert(false);
#endif
		return -1;
	}
	if(m_bkbufDxgiSurface!=NULL)
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 lock already and do not unlock.\n");
		assert(false);
#endif
		return -2;
	}

	HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(IDXGISurface1), (void**)&m_bkbufDxgiSurface);

	if (SUCCEEDED(hr))
	{
		hr = m_bkbufDxgiSurface->GetDC(FALSE, outHDC);
		if(SUCCEEDED(hr))
		{
			return 0;
		}
		else
		{
			ReleaseCOM(m_bkbufDxgiSurface);
		}
	}
	return -2;
}

int zRender::DxRender_D3D11::unlockBackbufferHDC( HDC hdc )
{
	if(m_device==NULL || m_depthView==NULL)
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 Object have not be inited yet.\n");
		assert(false);
#endif
		return -1;
	}
	if(m_bkbufDxgiSurface)
	{
		m_bkbufDxgiSurface->ReleaseDC(NULL);
		m_context->OMSetRenderTargets(1, &m_renderTargetView, m_depthView);
		ReleaseCOM(m_bkbufDxgiSurface);
	}
	return 0;
}

void* zRender::DxRender_D3D11::getDevice() const
{
	return (void*)m_device;
}

int zRender::DxRender_D3D11::getWidth()
{
	return m_winWidth;
}

int zRender::DxRender_D3D11::getHeight()
{
	return m_winHeight;
}

#include "DxRender_D3D9.h"
#include <D3DX11tex.h>
int zRender::DxRender_D3D11::getSnapshot( unsigned char* pData, UINT& datalen, int& w, int& h, int& pixfmt, int& pitch )
{
	return 0;
	//这个方法的实现性能并不是很高，所以当DxRender_D3D11对象渲染的目标分辨率>=1208*720时，以25fps的帧率调用此接口可能导致CPU上升2~4%（I5）
	if(m_device==NULL || m_depthView==NULL || NULL==m_context)
	{
#ifdef _DEBUG
		printf("DxRender_D3D11 Object have not be inited yet.\n");
		assert(false);
#endif
		return -1;
	}
	ID3D11Texture2D* backBuffer = getRenderTargetTexture();
	if(NULL==backBuffer)
	{
		return -6;
	}
	
	D3D11_TEXTURE2D_DESC backbufDesc;
	backBuffer->GetDesc(&backbufDesc);
	//用BackBuffer的DESC作为基础创建STAGE模式的TEXTURE2D用于将BackBuffer的内容拷贝出来
	backbufDesc.MipLevels = backbufDesc.ArraySize = 1;
	backbufDesc.SampleDesc.Count = 1;
	backbufDesc.Usage = D3D11_USAGE_STAGING;		//staging mode is more faster
	backbufDesc.BindFlags = 0;
	backbufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	backbufDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	ID3D11Texture2D* outputTexture = NULL;
	if(FAILED(m_device->CreateTexture2D( &backbufDesc, NULL, &outputTexture)))
	{
		ReleaseCOM(backBuffer);
		return -3;
	}
	m_context->CopyResource(outputTexture, backBuffer);
	HRESULT hr = D3DX11SaveTextureToFile(m_context, outputTexture, D3DX11_IFF_JPG, L"11tex.jpg");

	D3D11_MAPPED_SUBRESOURCE resource;
	unsigned int subresource = D3D11CalcSubresource(0, 0, 0);
	hr = m_context->Map(outputTexture, subresource, D3D11_MAP_READ/*D3D11_MAP_READ_WRITE*/, 0, &resource);
	//resource.pData; // TEXTURE DATA IS HERE
	if (FAILED(hr))
	{
		ReleaseCOM(outputTexture);
		ReleaseCOM(backBuffer);
		return -4;
	}
	//将数据从Texture中拷贝到调用者提供的内存中，并设置相关参数
	D3D11_TEXTURE2D_DESC outbufDesc;
	outputTexture->GetDesc(&outbufDesc);
	const int srcpitch = resource.RowPitch;
	const unsigned char* source = static_cast< const unsigned char* >(resource.pData);

	if(srcpitch*outbufDesc.Height > datalen || pData==NULL)
	{
		m_context->Unmap(outputTexture, subresource);
		ReleaseCOM(outputTexture);
		ReleaseCOM(backBuffer);
		return -5;
	}

// 		static unsigned int framecount = 0;
//  	char strFileName[128] = { 0 };
//  	sprintf(strFileName, "outfile2_%d_%d_%u.rgb", outbufDesc.Width, outbufDesc.Height, framecount);
//  	std::ofstream* imgfile = new std::ofstream(strFileName, std::ios::out | std::ios::binary);
//  	for (size_t i = 0; i < outbufDesc.Height; ++i)
//  	{
//  		const unsigned char* psrc = source + i*srcpitch;
//  		imgfile->write((const char*)psrc, srcpitch);
//  	}
//  	delete imgfile;
//	 	framecount++;
 	for (size_t i = 0; i < outbufDesc.Height; ++i)
 	{
 		const unsigned char* psrc = source + i*srcpitch;
 		unsigned char* pdst = pData + i*srcpitch;
		memcpy(pdst, psrc, srcpitch);
 	}
	w = outbufDesc.Width;
	h = outbufDesc.Height;
	pitch = srcpitch;
	datalen = outbufDesc.Height * srcpitch;
	pixfmt = (int)outbufDesc.Format;
	//释放相关对象
	m_context->Unmap(outputTexture, subresource);
	ReleaseCOM(outputTexture);
	ReleaseCOM(backBuffer);
	return 0;
}

int zRender::DxRender_D3D11::getSnapshot(SharedResource** outSharedTexture)
{
	if (NULL == outSharedTexture)
		return -1;
	ID3D11Texture2D* backBuffer;
	if(FAILED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
		return -2;
	D3D11_TEXTURE2D_DESC backbufDesc;
	backBuffer->GetDesc(&backbufDesc);
	int ret = -1;
	SharedResource* sharedRes = new SharedResource();
	ret = sharedRes->create(m_device, m_context, backbufDesc.Format, backbufDesc.Width, backbufDesc.Height, (int)D3D11_USAGE_DEFAULT, NULL, 0);
	if(0!=ret)
	{
		ReleaseCOM(backBuffer);
		delete sharedRes;
		return -3;
	}
	sharedRes->AcquireSync(INFINITE);
	ID3D11Texture2D* dstSharedTex = sharedRes->getTextureRes();
	m_context->CopyResource(dstSharedTex, backBuffer);
	//HRESULT hr = D3DX11SaveTextureToFile(m_context, dstSharedTex, D3DX11_IFF_DDS, L"11tex.dds");
	sharedRes->ReleaseSync();
	ReleaseCOM(backBuffer);

	*outSharedTexture = sharedRes;
	return 0;
}

TextureResource * zRender::DxRender_D3D11::getSnapshot(TEXTURE_USAGE usage, bool bShared, bool fromOffscreenTexture)
{
	if (TEXTURE_USAGE_STAGE == usage && bShared) return NULL;
	ID3D11Texture2D* renderTargetTex = getRenderTargetTexture();
	if (NULL == renderTargetTex)	return NULL;
	D3D11_TEXTURE2D_DESC desc;
	renderTargetTex->GetDesc(&desc);
	TextureResource* frameTexture = new TextureResource();
	if (0 != frameTexture->create(m_device, desc.Width, desc.Height, desc.Format, usage, bShared, NULL, 0, 0))
	{
		delete frameTexture;
		return NULL;
	}
	if (0 != frameTexture->copyTexture(renderTargetTex))
	{
		delete frameTexture;
	}
	if (NULL == m_renderTargetTexture && NULL==m_renderTargetBuffer)
	{
		ReleaseCOM(renderTargetTex);
	}
	return frameTexture;
}

int zRender::DxRender_D3D11::createOffscreenRenderTarget(int width, int height)
{
	if (m_device == NULL)
		return -1;
	if (width <= 0 || height <= 0 || width > 1920 * 4 || height > 1080 * 4)
		return -2;
	if (m_renderTargetTexture)	return -3;
	m_renderTargetTexture = new RenderTextureClass();
	if (!m_renderTargetTexture->Initialize(m_device, width, height))
	{
		return -4;
	}
	if (m_offscreenRttRender == NULL)
	{
		m_offscreenRttRender = new D3D11TextureRender(m_device, m_context);
	}
	return 0;
}

void zRender::DxRender_D3D11::releaseOffscreenRenderTarget()
{
	if (m_offscreenRttRender)
	{
		delete m_offscreenRttRender;
		m_offscreenRttRender = NULL;
	}
	if (m_renderTargetTexture)
	{
		m_renderTargetTexture->Shutdown();
		delete m_renderTargetTexture;
		m_renderTargetTexture = NULL;
	}
}

int zRender::DxRender_D3D11::setRenderTargetTexture()
{
	if (NULL == m_renderTargetTexture)
		return -1;
	m_renderTargetTexture->SetRenderTarget(m_context, m_depthView);
	m_viewport.TopLeftX = 0;
	m_viewport.TopLeftY = 0;
	m_viewport.Width = static_cast<float>(m_renderTargetTexture->GetWidth());
	m_viewport.Height = static_cast<float>(m_renderTargetTexture->GetHeight());
	m_viewport.MinDepth = 0.0f;
	m_viewport.MaxDepth = 1.0f;

	m_context->RSSetViewports(1, &m_viewport);
	return 0;
}

int zRender::DxRender_D3D11::setRenderTargetBackbuffer()
{
	if (m_swapChain == NULL || m_renderTargetView==NULL)
		return -1;
	m_context->OMSetRenderTargets(1, &m_renderTargetView, m_depthView);
	m_viewport.TopLeftX = 0;
	m_viewport.TopLeftY = 0;
	m_viewport.Width = static_cast<float>(m_winWidth);
	m_viewport.Height = static_cast<float>(m_winHeight);
	m_viewport.MinDepth = 0.0f;
	m_viewport.MaxDepth = 1.0f;

	m_context->RSSetViewports(1, &m_viewport);
	return 0;
}

int zRender::DxRender_D3D11::drawOffscreenRenderTarget()
{
	if (NULL == m_renderTargetTexture)
		return -1;
	if (m_offscreenRttRender == NULL)
		return -2;
	RECT rect;
	rect.left = rect.top = 0;
	rect.right = getWidth();
	rect.bottom = getHeight();
	//return m_offscreenRttRender->draw(m_renderTargetTexture->getRenderTargetTexture(), m_renderTargetTexture->GetShaderResourceView(), rect, m_swapChain); //fixme 这个调用可能崩溃
	return m_offscreenRttRender->draw(m_renderTargetTexture->getRenderTargetTexture(), m_renderTargetTexture->GetShaderResourceView(), rect, 1, &m_renderTargetView);
}

int zRender::DxRender_D3D11::clearBackbuffer(DWORD color)
{
	int a = (color & 0xff000000) >> 24;
	int r = (color & 0x00ff0000) >> 16;
	int g = (color & 0x0000ff00) >> 8;
	int b = (color & 0x000000ff) >> 0;
	float colorBack[4] = { (float)r / 256, (float)g / 256, (float)b / 256, (float)a / 256 };
	m_context->ClearRenderTargetView(m_renderTargetView, reinterpret_cast<const float*>(colorBack));
	m_context->ClearDepthStencilView(m_depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	return 0;
}

ID3D11Texture2D * zRender::DxRender_D3D11::getRenderTargetTexture()
{
	if (m_renderTargetTexture)
	{
		return m_renderTargetTexture->getRenderTargetTexture();
	}
	else if(m_renderTargetBuffer)
	{
		return m_renderTargetBuffer;
	}
	else
	{
		if (NULL == m_swapChain)	return NULL;
		ID3D11Texture2D* backBuffer;
		if (S_OK != m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))
			return NULL;
		return backBuffer;
	}
}

int zRender::DxRender_D3D11::resize( int new_width, int new_height )
{
	if(new_width<=0 || new_height<=0)
		return -1;
	assert(m_context);
	assert(m_device);
	assert(m_swapChain);
	if(m_context==NULL || NULL==m_device || (NULL==m_swapChain && m_renderTargetBuffer==NULL))
	{
		return -2;
	}
	// Release the old views, as they hold references to the buffers we
	// will be destroying.  Also release the old depth/stencil buffer.

	ReleaseCOM(m_renderTargetBuffer);
	ReleaseCOM(m_bkbufDxgiSurface);
	ReleaseCOM(m_renderTargetView);
	ReleaseCOM(m_depthView);
	ReleaseCOM(m_depthBuffer);

	ID3D11Texture2D* backBuffer = NULL;
	D3D11_TEXTURE2D_DESC textureDesc;
	D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc;
	if(NULL!=m_hWnd)
	{
		// Resize the swap chain and recreate the render target view.
		DXGI_SWAP_CHAIN_DESC sd_desc;
		m_swapChain->GetDesc(&sd_desc);
		HRESULT rslt = m_swapChain->ResizeBuffers(0, new_width, new_height, DXGI_FORMAT_UNKNOWN, sd_desc.Flags);
		if(FAILED(rslt))
		{
			return -3;
		}
		if(FAILED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
		{
			return -4;
		}
		if(NULL==backBuffer)
		{
			return -5;
		}
		backBuffer->GetDesc(&textureDesc);
	}
	else
	{
		HRESULT result;
		D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc;

		// Initialize the render target texture description.
		ZeroMemory(&textureDesc, sizeof(textureDesc));

		// Setup the render target texture description.
		textureDesc.Width = new_width;
		textureDesc.Height = new_height;
		textureDesc.MipLevels = 1;
		textureDesc.ArraySize = 1;
		textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Usage = D3D11_USAGE_DEFAULT;
		textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		textureDesc.CPUAccessFlags = 0;
		textureDesc.MiscFlags = 0;

		// Create the render target texture.
		result = m_device->CreateTexture2D(&textureDesc, NULL, &m_renderTargetBuffer);
		if(FAILED(result))
		{
			TCHAR errmsg[512] = {0};
			swprintf_s(errmsg, 512, L"Error in DxRender_D3D11::resize : create Texture2D for render target failed.");
			return -7;
		}
	}
	
	// Setup the description of the render target view.
	renderTargetViewDesc.Format = textureDesc.Format;
	renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	renderTargetViewDesc.Texture2D.MipSlice = 0;
	if(FAILED(m_device->CreateRenderTargetView(backBuffer, &renderTargetViewDesc, &m_renderTargetView)))
	{
		ReleaseCOM(backBuffer);
		return -5;
	}
	if(m_hWnd)
	{
		//从swapchain中获取的backbuffer需要release，不然可能内存泄露
		ReleaseCOM(backBuffer);
	}

	// Create the depth/stencil buffer and view.
	UINT i4xMsaaQuality = 0;
	if(S_OK!=m_device->CheckMultisampleQualityLevels(textureDesc.Format, 4, &i4xMsaaQuality))
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : CheckMultisampleQualityLevels failed.\n");
#endif
	}
	if( i4xMsaaQuality<=0 )
	{
#ifdef _DEBUG
		printf("Error in DxRender_D3D11::init : check quality support failed\n");
#endif
		return -8;
	}
	D3D11_TEXTURE2D_DESC depthStencilDesc;

	depthStencilDesc.Width     = new_width;
	depthStencilDesc.Height    = new_height;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.ArraySize = 1;
	depthStencilDesc.Format    = DXGI_FORMAT_D24_UNORM_S8_UINT;

	// Use 4X MSAA? --must match swap chain MSAA values.
	if( m_bEnable4xMsaa )
	{
		depthStencilDesc.SampleDesc.Count   = 4;
		depthStencilDesc.SampleDesc.Quality = i4xMsaaQuality-1;
	}
	// No MSAA
	else
	{
		depthStencilDesc.SampleDesc.Count   = 1;
		depthStencilDesc.SampleDesc.Quality = 0;
	}

	depthStencilDesc.Usage          = D3D11_USAGE_DEFAULT;
	depthStencilDesc.BindFlags      = D3D11_BIND_DEPTH_STENCIL;
	depthStencilDesc.CPUAccessFlags = 0; 
	depthStencilDesc.MiscFlags      = 0;

	if(FAILED(m_device->CreateTexture2D(&depthStencilDesc, 0, &m_depthBuffer)))
	{
		return -6;
	}
	if(FAILED(m_device->CreateDepthStencilView(m_depthBuffer, 0, &m_depthView)))
	{
		return -7;
	}


	// Bind the render target view and depth/stencil view to the pipeline.

	m_context->OMSetRenderTargets(1, &m_renderTargetView, m_depthView);


	// Set the viewport transform.

	m_viewport.TopLeftX = 0;
	m_viewport.TopLeftY = 0;
	m_viewport.Width    = static_cast<float>(new_width);
	m_viewport.Height   = static_cast<float>(new_height);
	m_viewport.MinDepth = 0.0f;
	m_viewport.MaxDepth = 1.0f;

	m_context->RSSetViewports(1, &m_viewport);
	m_winWidth = new_width;
	m_winHeight = new_height;
	return 0;
}

int zRender::DxRender_D3D11::createSharedTexture(SharedTexture** ppSharedTex, PIXFormat pixfmt)
{
	if(ppSharedTex==NULL)
		return -1;
	if(m_context==NULL || NULL==m_device)
	{
		return -2;
	}
	switch(pixfmt)
	{
	case PIXFMT_YUV420P:
	case PIXFMT_YV12:
		*ppSharedTex = new SharedTexture_I420P(m_device, m_context);
		return 0;
	case PIXFMT_A8R8G8B8:
	case PIXFMT_R8G8B8:
	case PIXFMT_X8R8G8B8:
	case PIXFMT_R8G8B8A8:
	case PIXFMT_B8G8R8A8:
	case PIXFMT_B8G8R8X8:
		*ppSharedTex = new SharedTexture_ARGB8(m_device, m_context);
		return 0;
	default:
		*ppSharedTex = NULL;
		return -3;
	}
	return -4;
}

int zRender::DxRender_D3D11::releaseSharedTexture(SharedTexture** ppSharedTex)
{
	if(ppSharedTex && *ppSharedTex)
	{
		SharedTexture* ptex = *ppSharedTex;
		ptex->release();
		delete ptex;
		*ppSharedTex = NULL;
		return 0;
	}
	return -1;
}

int zRender::DxRender_D3D11::createTextureResource(TextureResource ** ppOutTexRes, int width, int height, DXGI_FORMAT dxgiFmt, TEXTURE_USAGE usage, bool bShared, const char * initData, int dataLen, int pitch)
{
	if (ppOutTexRes == NULL)	return -1;
	TextureResource* frameTexture = new TextureResource();
	if (0 != frameTexture->create(m_device, width, height, dxgiFmt, usage, bShared, initData, dataLen, pitch))
	{
		delete frameTexture;
		return -2;
	}
	*ppOutTexRes = frameTexture;
	return 0;
}

int zRender::DxRender_D3D11::openSharedTextureResource(TextureResource ** ppOutTexRes, HANDLE hSharedRes)
{
	if (ppOutTexRes == NULL)	return -1;
	TextureResource* frameTexture = new TextureResource();
	if (0 != frameTexture->open(m_device, hSharedRes))
	{
		delete frameTexture;
	}
	*ppOutTexRes = frameTexture;
	return 0;
}

void zRender::DxRender_D3D11::releaseTextureResource(TextureResource ** ppOutTexRes)
{
	if (ppOutTexRes)
	{
		TextureResource* pTexRes = *ppOutTexRes;
		if (pTexRes)
		{
			printf("do DxRender_D3D11::releaseTextureResource\n");
			pTexRes->release();
			delete pTexRes;
			*ppOutTexRes = NULL;
		}
	}
}
