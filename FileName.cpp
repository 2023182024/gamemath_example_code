#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// =========================================================================
// 1. C++ 구조체 정의
// =========================================================================
struct Vertex
{
    XMFLOAT3 Pos;    // POSITION
    XMFLOAT3 Normal; // NORMAL
    XMFLOAT2 Tex;    // TEXCOORD0
};

struct TransformBuffer
{
    XMMATRIX WorldViewProjection;
    XMMATRIX WorldMatrix;
};

struct LightBuffer
{
    XMFLOAT3 LightDirection;
    float Padding1;
    XMFLOAT4 LightColor;
    XMFLOAT4 AmbientColor;
};

// =========================================================================
// 2. HLSL 셰이더 코드 (RGB 텍스처 샘플링 추가)
// =========================================================================
const char* shaderSource = R"(
cbuffer TransformBuffer : register(b0)
{
    matrix WorldViewProjection;
    matrix WorldMatrix;
};

cbuffer LightBuffer : register(b1)
{
    float3 LightDirection;
    float Padding1;
    float4 LightColor;
    float4 AmbientColor;
};

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

struct VSInput
{
    float4 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    
    output.position = mul(input.position, WorldViewProjection);

    float3 worldNormal = normalize(mul(input.normal, (float3x3)WorldMatrix));
    float3 lightDir = normalize(-LightDirection);

    float NdotL = saturate(dot(worldNormal, lightDir));
    
    // 정점 조명 계산값 전달
    output.color = AmbientColor + (LightColor * NdotL);
    output.uv = input.uv;

    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // RGB 텍스처 샘플링
    float4 texColor = g_Texture.Sample(g_Sampler, input.uv);
    
    // 정점 조명 결과와 텍스처 RGB 색상 결합
    return texColor * input.color;
}
)";

// =========================================================================
// 3. 전역 변수
// =========================================================================
HWND                    g_hWnd = NULL;
ID3D11Device* g_pd3dDevice = NULL;
ID3D11DeviceContext* g_pImmediateContext = NULL;
IDXGISwapChain* g_pSwapChain = NULL;
ID3D11RenderTargetView* g_pRenderTargetView = NULL;
ID3D11Texture2D* g_pDepthStencilBuffer = NULL;
ID3D11DepthStencilView* g_pDepthStencilView = NULL;

ID3D11VertexShader* g_pVertexShader = NULL;
ID3D11PixelShader* g_pPixelShader = NULL;
ID3D11InputLayout* g_pVertexLayout = NULL;

ID3D11Buffer* g_pVertexBuffer = NULL;
ID3D11Buffer* g_pIndexBuffer = NULL;
UINT                    g_IndexCount = 0;

ID3D11Buffer* g_pConstantBufferTransform = NULL;
ID3D11Buffer* g_pConstantBufferLight = NULL;

// 텍스처 및 샘플러 자원
ID3D11ShaderResourceView* g_pTextureSRV = NULL;
ID3D11SamplerState* g_pSamplerState = NULL;

// 카메라 회전 변수 (우클릭 드래그)
float g_CameraYaw = 0.0f;
float g_CameraPitch = 0.0f;
float g_CameraRadius = 5.0f;

// 구체 물체 회전 변수 (좌클릭 드래그)
float g_SphereYaw = 0.0f;
float g_SpherePitch = 0.0f;

POINT g_LastMousePos;
bool  g_IsLMouseDown = false;
bool  g_IsRMouseDown = false;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// =========================================================================
// 4. 동적 RGB 체크보드 텍스처 생성
// =========================================================================
HRESULT CreateRGBTexture()
{
    const int texWidth = 256;
    const int texHeight = 256;
    std::vector<UINT32> pixels(texWidth * texHeight);

    // RGB 격자 패턴 생성
    for (int y = 0; y < texHeight; ++y)
    {
        for (int x = 0; x < texWidth; ++x)
        {
            int checkX = x / 32;
            int checkY = y / 32;

            if ((checkX + checkY) % 3 == 0)
                pixels[y * texWidth + x] = 0xFF0000FF; // Red (ABGR)
            else if ((checkX + checkY) % 3 == 1)
                pixels[y * texWidth + x] = 0xFF00FF00; // Green
            else
                pixels[y * texWidth + x] = 0xFFFF0000; // Blue
        }
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = texWidth;
    texDesc.Height = texHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = texWidth * sizeof(UINT32);

    ID3D11Texture2D* pTex = NULL;
    HRESULT hr = g_pd3dDevice->CreateTexture2D(&texDesc, &initData, &pTex);
    if (FAILED(hr)) return hr;

    hr = g_pd3dDevice->CreateShaderResourceView(pTex, NULL, &g_pTextureSRV);
    pTex->Release();
    if (FAILED(hr)) return hr;

    // 샘플러 스테이트 생성
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;

    return g_pd3dDevice->CreateSamplerState(&sampDesc, &g_pSamplerState);
}

// =========================================================================
// 5. 구(Sphere) 메쉬 생성
// =========================================================================
void CreateSphere(float radius, UINT sliceCount, UINT stackCount, std::vector<Vertex>& vertices, std::vector<UINT>& indices)
{
    vertices.clear();
    indices.clear();

    Vertex topVertex = { XMFLOAT3(0.0f, radius, 0.0f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) };
    vertices.push_back(topVertex);

    float phiStep = XM_PI / stackCount;
    float thetaStep = XM_2PI / sliceCount;

    for (UINT i = 1; i <= stackCount - 1; ++i)
    {
        float phi = i * phiStep;
        for (UINT j = 0; j <= sliceCount; ++j)
        {
            float theta = j * thetaStep;

            Vertex v;
            v.Pos.x = radius * sinf(phi) * cosf(theta);
            v.Pos.y = radius * cosf(phi);
            v.Pos.z = radius * sinf(phi) * sinf(theta);

            XMVECTOR p = XMLoadFloat3(&v.Pos);
            XMStoreFloat3(&v.Normal, XMVector3Normalize(p));

            v.Tex.x = theta / XM_2PI;
            v.Tex.y = phi / XM_PI;

            vertices.push_back(v);
        }
    }

    Vertex bottomVertex = { XMFLOAT3(0.0f, -radius, 0.0f), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) };
    vertices.push_back(bottomVertex);

    for (UINT i = 1; i <= sliceCount; ++i)
    {
        indices.push_back(0);
        indices.push_back(i + 1);
        indices.push_back(i);
    }

    UINT baseIndex = 1;
    UINT ringVertexCount = sliceCount + 1;
    for (UINT i = 0; i < stackCount - 2; ++i)
    {
        for (UINT j = 0; j < sliceCount; ++j)
        {
            indices.push_back(baseIndex + i * ringVertexCount + j);
            indices.push_back(baseIndex + i * ringVertexCount + j + 1);
            indices.push_back(baseIndex + (i + 1) * ringVertexCount + j);

            indices.push_back(baseIndex + (i + 1) * ringVertexCount + j);
            indices.push_back(baseIndex + i * ringVertexCount + j + 1);
            indices.push_back(baseIndex + (i + 1) * ringVertexCount + j + 1);
        }
    }

    UINT southPoleIndex = (UINT)vertices.size() - 1;
    baseIndex = southPoleIndex - ringVertexCount;
    for (UINT i = 0; i < sliceCount; ++i)
    {
        indices.push_back(southPoleIndex);
        indices.push_back(baseIndex + i);
        indices.push_back(baseIndex + i + 1);
    }
}

// =========================================================================
// 6. DirectX 11 및 자원 초기화
// =========================================================================
HRESULT InitDevice()
{
    HRESULT hr = S_OK;

    RECT rc;
    GetClientRect(g_hWnd, &rc);
    UINT width = rc.right - rc.left;
    UINT height = rc.bottom - rc.top;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, NULL, &g_pImmediateContext);
    if (FAILED(hr)) return hr;

    ID3D11Texture2D* pBackBuffer = NULL;
    hr = g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    if (FAILED(hr)) return hr;

    hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_pRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = g_pd3dDevice->CreateTexture2D(&depthDesc, NULL, &g_pDepthStencilBuffer);
    if (FAILED(hr)) return hr;

    hr = g_pd3dDevice->CreateDepthStencilView(g_pDepthStencilBuffer, NULL, &g_pDepthStencilView);
    if (FAILED(hr)) return hr;

    g_pImmediateContext->OMSetRenderTargets(1, &g_pRenderTargetView, g_pDepthStencilView);

    D3D11_VIEWPORT vp;
    vp.Width = (FLOAT)width;
    vp.Height = (FLOAT)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    g_pImmediateContext->RSSetViewports(1, &vp);

    ID3DBlob* pVSBlob = NULL;
    ID3DBlob* pErrorBlob = NULL;
    hr = D3DCompile(shaderSource, strlen(shaderSource), NULL, NULL, NULL, "VSMain", "vs_5_0", 0, 0, &pVSBlob, &pErrorBlob);
    if (FAILED(hr)) return hr;

    hr = g_pd3dDevice->CreateVertexShader(pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), NULL, &g_pVertexShader);
    if (FAILED(hr)) return hr;

    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = g_pd3dDevice->CreateInputLayout(layout, ARRAYSIZE(layout), pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), &g_pVertexLayout);
    pVSBlob->Release();
    if (FAILED(hr)) return hr;

    ID3DBlob* pPSBlob = NULL;
    hr = D3DCompile(shaderSource, strlen(shaderSource), NULL, NULL, NULL, "PSMain", "ps_5_0", 0, 0, &pPSBlob, &pErrorBlob);
    if (FAILED(hr)) return hr;

    hr = g_pd3dDevice->CreatePixelShader(pPSBlob->GetBufferPointer(), pPSBlob->GetBufferSize(), NULL, &g_pPixelShader);
    pPSBlob->Release();
    if (FAILED(hr)) return hr;

    std::vector<Vertex> vertices;
    std::vector<UINT> indices;
    CreateSphere(1.5f, 30, 30, vertices, indices);
    g_IndexCount = (UINT)indices.size();

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(Vertex) * (UINT)vertices.size();
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA InitData = {};
    InitData.pSysMem = vertices.data();
    hr = g_pd3dDevice->CreateBuffer(&bd, &InitData, &g_pVertexBuffer);
    if (FAILED(hr)) return hr;

    bd.ByteWidth = sizeof(UINT) * g_IndexCount;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    InitData.pSysMem = indices.data();
    hr = g_pd3dDevice->CreateBuffer(&bd, &InitData, &g_pIndexBuffer);
    if (FAILED(hr)) return hr;

    bd.ByteWidth = sizeof(TransformBuffer);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_pd3dDevice->CreateBuffer(&bd, NULL, &g_pConstantBufferTransform);
    if (FAILED(hr)) return hr;

    bd.ByteWidth = sizeof(LightBuffer);
    hr = g_pd3dDevice->CreateBuffer(&bd, NULL, &g_pConstantBufferLight);
    if (FAILED(hr)) return hr;

    // RGB 텍스처 초기화
    hr = CreateRGBTexture();
    if (FAILED(hr)) return hr;

    return S_OK;
}

// =========================================================================
// 7. 렌더링 루프
// =========================================================================
void Render()
{
    float ClearColor[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
    g_pImmediateContext->ClearRenderTargetView(g_pRenderTargetView, ClearColor);
    g_pImmediateContext->ClearDepthStencilView(g_pDepthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);

    // 우클릭 마우스 드래그 기반 카메라 행렬
    float camX = g_CameraRadius * cosf(g_CameraPitch) * sinf(g_CameraYaw);
    float camY = g_CameraRadius * sinf(g_CameraPitch);
    float camZ = g_CameraRadius * cosf(g_CameraPitch) * cosf(g_CameraYaw);

    XMVECTOR eyePos = XMVectorSet(camX, camY, camZ, 0.0f);
    XMVECTOR targetPos = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
    XMVECTOR upDir = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(eyePos, targetPos, upDir);

    RECT rc;
    GetClientRect(g_hWnd, &rc);
    float aspect = (float)(rc.right - rc.left) / (float)(rc.bottom - rc.top);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.01f, 100.0f);

    // 좌클릭 마우스 드래그 기반 구체(월드) 회전 행렬
    XMMATRIX world = XMMatrixRotationRollPitchYaw(g_SpherePitch, g_SphereYaw, 0.0f);

    TransformBuffer cbTransform;
    cbTransform.WorldMatrix = XMMatrixTranspose(world);
    cbTransform.WorldViewProjection = XMMatrixTranspose(world * view * proj);
    g_pImmediateContext->UpdateSubresource(g_pConstantBufferTransform, 0, NULL, &cbTransform, 0, 0);

    LightBuffer cbLight;
    cbLight.LightDirection = XMFLOAT3(-1.0f, -1.0f, 1.0f);
    cbLight.LightColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    cbLight.AmbientColor = XMFLOAT4(0.2f, 0.2f, 0.2f, 1.0f);
    g_pImmediateContext->UpdateSubresource(g_pConstantBufferLight, 0, NULL, &cbLight, 0, 0);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    g_pImmediateContext->IASetVertexBuffers(0, 1, &g_pVertexBuffer, &stride, &offset);
    g_pImmediateContext->IASetIndexBuffer(g_pIndexBuffer, DXGI_FORMAT_R32_UINT, 0);
    g_pImmediateContext->IASetInputLayout(g_pVertexLayout);
    g_pImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    g_pImmediateContext->VSSetShader(g_pVertexShader, NULL, 0);
    g_pImmediateContext->VSSetConstantBuffers(0, 1, &g_pConstantBufferTransform);
    g_pImmediateContext->VSSetConstantBuffers(1, 1, &g_pConstantBufferLight);

    g_pImmediateContext->PSSetShader(g_pPixelShader, NULL, 0);
    g_pImmediateContext->PSSetShaderResources(0, 1, &g_pTextureSRV);
    g_pImmediateContext->PSSetSamplers(0, 1, &g_pSamplerState);

    g_pImmediateContext->DrawIndexed(g_IndexCount, 0, 0);
    g_pSwapChain->Present(0, 0);
}

// =========================================================================
// 8. 메인 진입점 및 마우스 좌/우클릭 이벤트 제어
// =========================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEX wcex = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"TexturedSphereWindow", NULL };
    RegisterClassEx(&wcex);

    g_hWnd = CreateWindow(L"TexturedSphereWindow", L"RGB Textured Sphere - Left Drag: Rotate Sphere / Right Drag: Rotate Camera",
        WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, NULL, NULL, wcex.hInstance, NULL);

    if (FAILED(InitDevice())) return 0;

    ShowWindow(g_hWnd, nCmdShow);

    MSG msg = { 0 };
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            Render();
        }
    }

    if (g_pSamplerState) g_pSamplerState->Release();
    if (g_pTextureSRV) g_pTextureSRV->Release();
    if (g_pDepthStencilView) g_pDepthStencilView->Release();
    if (g_pDepthStencilBuffer) g_pDepthStencilBuffer->Release();
    if (g_pConstantBufferLight) g_pConstantBufferLight->Release();
    if (g_pConstantBufferTransform) g_pConstantBufferTransform->Release();
    if (g_pIndexBuffer) g_pIndexBuffer->Release();
    if (g_pVertexBuffer) g_pVertexBuffer->Release();
    if (g_pVertexLayout) g_pVertexLayout->Release();
    if (g_pVertexShader) g_pVertexShader->Release();
    if (g_pPixelShader) g_pPixelShader->Release();
    if (g_pRenderTargetView) g_pRenderTargetView->Release();
    if (g_pSwapChain) g_pSwapChain->Release();
    if (g_pImmediateContext) g_pImmediateContext->Release();
    if (g_pd3dDevice) g_pd3dDevice->Release();

    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_LBUTTONDOWN:
        g_IsLMouseDown = true;
        g_LastMousePos.x = LOWORD(lParam);
        g_LastMousePos.y = HIWORD(lParam);
        SetCapture(hWnd);
        return 0;

    case WM_LBUTTONUP:
        g_IsLMouseDown = false;
        if (!g_IsRMouseDown) ReleaseCapture();
        return 0;

    case WM_RBUTTONDOWN:
        g_IsRMouseDown = true;
        g_LastMousePos.x = LOWORD(lParam);
        g_LastMousePos.y = HIWORD(lParam);
        SetCapture(hWnd);
        return 0;

    case WM_RBUTTONUP:
        g_IsRMouseDown = false;
        if (!g_IsLMouseDown) ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
    {
        int dx = LOWORD(lParam) - g_LastMousePos.x;
        int dy = HIWORD(lParam) - g_LastMousePos.y;

        // 좌클릭: 물체(구) 회전
        if (g_IsLMouseDown)
        {
            g_SphereYaw += dx * 0.005f;
            g_SpherePitch += dy * 0.005f;
        }
        // 우클릭: 카메라 회전
        else if (g_IsRMouseDown)
        {
            g_CameraYaw += dx * 0.005f;
            g_CameraPitch += dy * 0.005f;
            g_CameraPitch = XMMin(XMMax(g_CameraPitch, -XM_PIDIV2 + 0.01f), XM_PIDIV2 - 0.01f);
        }

        g_LastMousePos.x = LOWORD(lParam);
        g_LastMousePos.y = HIWORD(lParam);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}