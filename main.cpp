// =====================================================================================
// Screen Color-Grading Overlay
//
// Effects:
//   Brightness
//   Contrast
//   Saturation
//   Vibrance
//   Gamma
//   Temperature
//   Bloom
//   Clarity
//   Sharpen
//
// Hotkeys:
//   INSERT  = toggle control panel
//   SHIFT+M = completely terminate the program
//
// Presets:
//   %APPDATA%\ScreenGradingOverlay\presets.ini
//
// Dependencies:
//   imgui.cpp
//   imgui_draw.cpp
//   imgui_tables.cpp
//   imgui_widgets.cpp
//   backends/imgui_impl_win32.cpp
//   backends/imgui_impl_dx11.cpp
//
// Link:
//   d3d11.lib
//   dxgi.lib
//   d3dcompiler.lib
//
// =====================================================================================

#define NOMINMAX

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND,
    UINT,
    WPARAM,
    LPARAM
);

// =====================================================================================
// GLOBALS
// =====================================================================================

static HWND g_hwnd = nullptr;

static ComPtr<ID3D11Device>          g_device;
static ComPtr<ID3D11DeviceContext>   g_context;
static ComPtr<IDXGISwapChain>        g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_rtv;

// Desktop duplication
static ComPtr<IDXGIOutputDuplication>  g_dupl;
static ComPtr<ID3D11Texture2D>          g_screenTex;
static ComPtr<ID3D11ShaderResourceView> g_screenSRV;

static UINT g_screenW = 0;
static UINT g_screenH = 0;

// Grading pipeline
static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader>  g_ps;
static ComPtr<ID3D11Buffer>       g_paramsCB;
static ComPtr<ID3D11SamplerState> g_sampler;

// UI state
static bool g_panelVisible = true;
static bool g_clickThrough = false;

// Hotkeys
static const int HOTKEY_TOGGLE_PANEL = 1;
static const int HOTKEY_KILLSWITCH = 2;

// =====================================================================================
// GRADING PARAMETERS
// =====================================================================================

struct GradeParams
{
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float vibrance = 0.0f;

    float gamma = 1.0f;
    float temperature = 0.0f;

    float bloom = 0.0f;
    float clarity = 0.0f;

    float sharpen = 0.0f;

    float screenWidth = 1920.0f;
    float screenHeight = 1080.0f;

    float pad0 = 0.0f;
};

static GradeParams g_params;

// =====================================================================================
// PRESETS
// =====================================================================================

struct Preset
{
    std::string name;
    GradeParams params;
};

static std::vector<Preset> g_presets;

static std::string g_autoLoadName;

static char g_presetNameBuf[128] = "";

static int g_selectedPreset = -1;

// =====================================================================================
// CONFIG PATH
// =====================================================================================

static std::string GetConfigDir()
{
    std::string dir = ".";

#ifdef _MSC_VER

    char* appdata = nullptr;
    size_t len = 0;

    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 &&
        appdata != nullptr)
    {
        dir =
            std::string(appdata) +
            "\\ScreenGradingOverlay";

        free(appdata);
    }

#else

    const char* appdata = std::getenv("APPDATA");

    if (appdata)
    {
        dir =
            std::string(appdata) +
            "\\ScreenGradingOverlay";
    }

#endif

    CreateDirectoryA(
        dir.c_str(),
        nullptr
    );

    return dir;
}

static std::string GetConfigPath()
{
    return GetConfigDir() +
        "\\presets.ini";
}

// =====================================================================================
// PRESET SAVE
// =====================================================================================

static void SavePresets()
{
    const std::string path =
        GetConfigPath();

    std::ofstream f(
        path,
        std::ios::out |
        std::ios::trunc
    );

    if (!f)
    {
        OutputDebugStringA(
            "ScreenGradingOverlay: "
            "failed to save presets.\n"
        );

        return;
    }

    f.precision(6);

    f << "[__meta__]\n";
    f << "autoload="
        << g_autoLoadName
        << "\n\n";

    for (const auto& p : g_presets)
    {
        f << "["
            << p.name
            << "]\n";

        f << "brightness="
            << p.params.brightness
            << "\n";

        f << "contrast="
            << p.params.contrast
            << "\n";

        f << "saturation="
            << p.params.saturation
            << "\n";

        f << "vibrance="
            << p.params.vibrance
            << "\n";

        f << "gamma="
            << p.params.gamma
            << "\n";

        f << "temperature="
            << p.params.temperature
            << "\n";

        f << "bloom="
            << p.params.bloom
            << "\n";

        f << "clarity="
            << p.params.clarity
            << "\n";

        f << "sharpen="
            << p.params.sharpen
            << "\n\n";
    }

    f.flush();
}

// =====================================================================================
// PRESET LOAD
// =====================================================================================

static void LoadPresets()
{
    g_presets.clear();

    g_autoLoadName.clear();

    g_selectedPreset = -1;

    std::ifstream f(
        GetConfigPath()
    );

    if (!f)
        return;

    std::string line;
    std::string section;

    Preset* current = nullptr;

    while (std::getline(f, line))
    {
        if (line.empty())
            continue;

        if (line.front() == '[' &&
            line.back() == ']')
        {
            section =
                line.substr(
                    1,
                    line.size() - 2
                );

            if (section == "__meta__")
            {
                current = nullptr;
            }
            else
            {
                g_presets.push_back(
                    Preset{
                        section,
                        GradeParams{}
                    }
                );

                current =
                    &g_presets.back();
            }

            continue;
        }

        const size_t eq =
            line.find('=');

        if (eq == std::string::npos)
            continue;

        const std::string key =
            line.substr(0, eq);

        const std::string val =
            line.substr(eq + 1);

        if (section == "__meta__")
        {
            if (key == "autoload")
                g_autoLoadName = val;

            continue;
        }

        if (!current)
            continue;

        const float v =
            static_cast<float>(
                atof(val.c_str())
                );

        if (key == "brightness")
            current->params.brightness = v;

        else if (key == "contrast")
            current->params.contrast = v;

        else if (key == "saturation")
            current->params.saturation = v;

        else if (key == "vibrance")
            current->params.vibrance = v;

        else if (key == "gamma")
            current->params.gamma = v;

        else if (key == "temperature")
            current->params.temperature = v;

        else if (key == "bloom")
            current->params.bloom = v;

        else if (key == "clarity")
            current->params.clarity = v;

        else if (key == "sharpen")
            current->params.sharpen = v;
    }
}

// =====================================================================================
// SAVE SINGLE PRESET
// =====================================================================================

static void SavePresetAs(
    const std::string& name
)
{
    if (name.empty())
        return;

    for (auto& p : g_presets)
    {
        if (p.name == name)
        {
            p.params = g_params;

            SavePresets();

            return;
        }
    }

    g_presets.push_back(
        Preset{
            name,
            g_params
        }
    );

    SavePresets();
}

// =====================================================================================
// DELETE PRESET
// =====================================================================================

static void DeletePreset(
    int index
)
{
    if (index < 0 ||
        index >= static_cast<int>(
            g_presets.size()
            ))
    {
        return;
    }

    if (g_presets[index].name ==
        g_autoLoadName)
    {
        g_autoLoadName.clear();
    }

    g_presets.erase(
        g_presets.begin() + index
    );

    g_selectedPreset = -1;

    SavePresets();
}

// =====================================================================================
// AUTO LOAD
// =====================================================================================

static void ApplyAutoLoadIfAny()
{
    if (g_autoLoadName.empty())
        return;

    for (const auto& p : g_presets)
    {
        if (p.name ==
            g_autoLoadName)
        {
            g_params = p.params;

            return;
        }
    }
}

// =====================================================================================
// VERTEX SHADER
// =====================================================================================

static const char* kVertexShaderSrc = R"(
struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    VSOut o;

    float2 uv =
        float2(
            (id << 1) & 2,
            id & 2
        );

    o.uv = uv;

    o.pos =
        float4(
            uv * float2(2, -2) +
            float2(-1, 1),
            0,
            1
        );

    return o;
}
)";

// =====================================================================================
// PIXEL SHADER
// =====================================================================================

static const char* kPixelShaderSrc = R"(
Texture2D screenTex : register(t0);

SamplerState samp : register(s0);

cbuffer Params : register(b0)
{
    float brightness;
    float contrast;
    float saturation;
    float vibrance;

    float gammaVal;
    float temperature;

    float bloom;
    float clarity;

    float sharpen;

    float screenWidth;
    float screenHeight;

    float pad0;
};


// ============================================================================
// TEMPERATURE
// ============================================================================

float3 applyTemperature(
    float3 c,
    float t
)
{
    c.r =
        saturate(
            c.r +
            t * 0.10
        );

    c.b =
        saturate(
            c.b -
            t * 0.10
        );

    return c;
}


// ============================================================================
// VIBRANCE
// ============================================================================

float3 applyVibrance(
    float3 c,
    float v
)
{
    float maxc =
        max(
            c.r,
            max(c.g, c.b)
        );

    float avgc =
        (c.r + c.g + c.b)
        / 3.0;

    float sat =
        maxc - avgc;

    float amt =
        (1.0 - sat) * v;

    float luma =
        dot(
            c,
            float3(
                0.2126,
                0.7152,
                0.0722
            )
        );

    return lerp(
        float3(luma, luma, luma),
        c,
        1.0 + amt
    );
}


// ============================================================================
// 3x3 BLUR
// ============================================================================

float3 SampleBlur(
    float2 uv,
    float2 texel
)
{
    float3 result = 0.0;

    result +=
        screenTex.Sample(
            samp,
            uv +
            texel *
            float2(-1, -1)
        ).rgb;

    result +=
        screenTex.Sample(
            samp,
            uv +
            texel *
            float2(0, -1)
        ).rgb;

    result +=
        screenTex.Sample(
            samp,
            uv +
            texel *
            float2(1, -1)
        ).rgb;

    result +=
        screenTex.Sample(
            samp,
            uv +
            texel *
            float2(-1, 0)
        ).rgb;

    result +=
        screenTex.Sample(
            samp,
            uv
        ).rgb;

    result +=
        screenTex.Sample(
            samp,
            uv +
            texel *
            float2(1, 0)
        ).rgb;

    result +=
        screenTex.Sample(
            samp,
            uv +
            texel *
            float2(-1, 1)
        ).rgb;

    result +=
        screenTex.Sample(
            samp,
            uv +
            texel *
            float2(0, 1)
        ).rgb;

    result +=
        screenTex.Sample(
            samp,
            uv +
            texel *
            float2(1, 1)
        ).rgb;

    return result / 9.0;
}


// ============================================================================
// BLOOM
// ============================================================================

float3 SampleBloom(
    float2 uv,
    float2 texel
)
{
    float3 sum = 0.0;

    float2 offsets[8] =
    {
        float2(-2, -2),
        float2( 0, -2),
        float2( 2, -2),

        float2(-2,  0),
        float2( 2,  0),

        float2(-2,  2),
        float2( 0,  2),
        float2( 2,  2)
    };

    for (int i = 0; i < 8; i++)
    {
        float3 c =
            screenTex.Sample(
                samp,
                uv +
                offsets[i] *
                texel
            ).rgb;

        float luminance =
            dot(
                c,
                float3(
                    0.2126,
                    0.7152,
                    0.0722
                )
            );

        float mask =
            smoothstep(
                0.55,
                0.95,
                luminance
            );

        sum +=
            c * mask;
    }

    return sum / 8.0;
}


// ============================================================================
// MAIN
// ============================================================================

float4 main(
    float4 pos : SV_POSITION,
    float2 uv : TEXCOORD0
) : SV_TARGET
{
    float2 texel =
        float2(
            1.0 / max(screenWidth, 1.0),
            1.0 / max(screenHeight, 1.0)
        );


    // ------------------------------------------------------------------------
    // Original screen
    // ------------------------------------------------------------------------

    float3 color =
        screenTex.Sample(
            samp,
            uv
        ).rgb;


    // ------------------------------------------------------------------------
    // Brightness
    // ------------------------------------------------------------------------

    color += brightness;


    // ------------------------------------------------------------------------
    // Contrast
    // ------------------------------------------------------------------------

    color =
        (color - 0.5) *
        contrast +
        0.5;


    // ------------------------------------------------------------------------
    // Saturation
    // ------------------------------------------------------------------------

    float luma =
        dot(
            color,
            float3(
                0.2126,
                0.7152,
                0.0722
            )
        );

    color =
        lerp(
            float3(
                luma,
                luma,
                luma
            ),
            color,
            saturation
        );


    // ------------------------------------------------------------------------
    // Vibrance
    // ------------------------------------------------------------------------

    color =
        applyVibrance(
            color,
            vibrance
        );


    // ------------------------------------------------------------------------
    // Temperature
    // ------------------------------------------------------------------------

    color =
        applyTemperature(
            color,
            temperature
        );


    // ------------------------------------------------------------------------
    // Clarity
    // ------------------------------------------------------------------------

    if (clarity > 0.001)
    {
        float3 blurred =
            SampleBlur(
                uv,
                texel * 2.0
            );

        float3 localDetail =
            color - blurred;

        color +=
            localDetail *
            clarity *
            1.25;
    }


    // ------------------------------------------------------------------------
    // Sharpen
    // ------------------------------------------------------------------------

    if (sharpen > 0.001)
    {
        float3 blurred =
            SampleBlur(
                uv,
                texel
            );

        float3 detail =
            color - blurred;

        color +=
            detail *
            sharpen *
            1.5;
    }


    // ------------------------------------------------------------------------
    // Bloom
    // ------------------------------------------------------------------------

    if (bloom > 0.001)
    {
        float3 bloomColor =
            SampleBloom(
                uv,
                texel * 2.5
            );

        color +=
            bloomColor *
            bloom *
            0.35;
    }


    // ------------------------------------------------------------------------
    // Gamma
    // ------------------------------------------------------------------------

    color =
        saturate(color);

    color =
        pow(
            color,
            1.0 /
            max(
                gammaVal,
                0.001
            )
        );


    return float4(
        saturate(color),
        1.0
    );
}
)";

// =====================================================================================
// CLICK THROUGH
// =====================================================================================

static void SetClickThrough(
    bool enable
)
{
    g_clickThrough = enable;

    LONG_PTR exStyle =
        GetWindowLongPtr(
            g_hwnd,
            GWL_EXSTYLE
        );

    if (enable)
    {
        exStyle |= WS_EX_TRANSPARENT;
    }
    else
    {
        exStyle &= ~WS_EX_TRANSPARENT;
    }

    SetWindowLongPtr(
        g_hwnd,
        GWL_EXSTYLE,
        exStyle
    );

    SetWindowPos(
        g_hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_NOZORDER |
        SWP_NOACTIVATE |
        SWP_FRAMECHANGED
    );
}

// =====================================================================================
// SHUTDOWN / KILL SWITCH
// =====================================================================================

static void KillApplication()
{
    // Immediately tell the message loop to terminate.
    PostMessage(
        g_hwnd,
        WM_CLOSE,
        0,
        0
    );
}

// =====================================================================================
// SHADER COMPILATION
// =====================================================================================

static bool CompileShaderFromString(
    const char* src,
    const char* entry,
    const char* target,
    ID3DBlob** outBlob
)
{
    ComPtr<ID3DBlob> errBlob;

    HRESULT hr =
        D3DCompile(
            src,
            strlen(src),
            nullptr,
            nullptr,
            nullptr,
            entry,
            target,
            0,
            0,
            outBlob,
            &errBlob
        );

    if (FAILED(hr))
    {
        if (errBlob)
        {
            OutputDebugStringA(
                static_cast<const char*>(
                    errBlob->GetBufferPointer()
                    )
            );
        }

        return false;
    }

    return true;
}

// =====================================================================================
// D3D INITIALIZATION
// =====================================================================================

static bool InitD3D(
    HWND hwnd
)
{
    DXGI_SWAP_CHAIN_DESC sd = {};

    sd.BufferCount = 2;

    sd.BufferDesc.Format =
        DXGI_FORMAT_R8G8B8A8_UNORM;

    sd.BufferUsage =
        DXGI_USAGE_RENDER_TARGET_OUTPUT;

    sd.OutputWindow =
        hwnd;

    sd.SampleDesc.Count = 1;

    sd.Windowed = TRUE;

    sd.SwapEffect =
        DXGI_SWAP_EFFECT_DISCARD;


    UINT flags = 0;

#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL fl;

    HRESULT hr =
        D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &sd,
            &g_swapChain,
            &g_device,
            &fl,
            &g_context
        );

    if (FAILED(hr))
        return false;


    ComPtr<ID3D11Texture2D> backBuffer;

    if (FAILED(
        g_swapChain->GetBuffer(
            0,
            IID_PPV_ARGS(
                &backBuffer
            )
        )
    ))
    {
        return false;
    }


    if (FAILED(
        g_device->CreateRenderTargetView(
            backBuffer.Get(),
            nullptr,
            &g_rtv
        )
    ))
    {
        return false;
    }

    return true;
}

// =====================================================================================
// DESKTOP DUPLICATION
// =====================================================================================

static bool InitDesktopDuplication()
{
    ComPtr<IDXGIDevice> dxgiDevice;

    if (FAILED(
        g_device.As(
            &dxgiDevice
        )
    ))
    {
        return false;
    }


    ComPtr<IDXGIAdapter> adapter;

    if (FAILED(
        dxgiDevice->GetAdapter(
            &adapter
        )
    ))
    {
        return false;
    }


    ComPtr<IDXGIOutput> output;

    if (FAILED(
        adapter->EnumOutputs(
            0,
            &output
        )
    ))
    {
        return false;
    }


    ComPtr<IDXGIOutput1> output1;

    if (FAILED(
        output.As(
            &output1
        )
    ))
    {
        return false;
    }


    DXGI_OUTPUT_DESC desc = {};

    if (FAILED(
        output->GetDesc(
            &desc
        )
    ))
    {
        return false;
    }


    g_screenW =
        static_cast<UINT>(
            desc.DesktopCoordinates.right -
            desc.DesktopCoordinates.left
            );

    g_screenH =
        static_cast<UINT>(
            desc.DesktopCoordinates.bottom -
            desc.DesktopCoordinates.top
            );


    if (FAILED(
        output1->DuplicateOutput(
            g_device.Get(),
            &g_dupl
        )
    ))
    {
        return false;
    }


    D3D11_TEXTURE2D_DESC texDesc = {};

    texDesc.Width =
        g_screenW;

    texDesc.Height =
        g_screenH;

    texDesc.MipLevels = 1;

    texDesc.ArraySize = 1;

    texDesc.Format =
        DXGI_FORMAT_B8G8R8A8_UNORM;

    texDesc.SampleDesc.Count = 1;

    texDesc.Usage =
        D3D11_USAGE_DEFAULT;

    texDesc.BindFlags =
        D3D11_BIND_SHADER_RESOURCE;


    if (FAILED(
        g_device->CreateTexture2D(
            &texDesc,
            nullptr,
            &g_screenTex
        )
    ))
    {
        return false;
    }


    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

    srvDesc.Format =
        texDesc.Format;

    srvDesc.ViewDimension =
        D3D11_SRV_DIMENSION_TEXTURE2D;

    srvDesc.Texture2D.MipLevels = 1;


    if (FAILED(
        g_device->CreateShaderResourceView(
            g_screenTex.Get(),
            &srvDesc,
            &g_screenSRV
        )
    ))
    {
        return false;
    }


    return true;
}

// =====================================================================================
// CAPTURE FRAME
// =====================================================================================

static bool CaptureFrame()
{
    if (!g_dupl)
        return false;


    ComPtr<IDXGIResource> desktopResource;

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};


    HRESULT hr =
        g_dupl->AcquireNextFrame(
            0,
            &frameInfo,
            &desktopResource
        );


    if (hr ==
        DXGI_ERROR_WAIT_TIMEOUT)
    {
        return false;
    }


    if (FAILED(hr))
    {
        return false;
    }


    ComPtr<ID3D11Texture2D> frameTex;


    if (FAILED(
        desktopResource.As(
            &frameTex
        )
    ))
    {
        g_dupl->ReleaseFrame();

        return false;
    }


    g_context->CopyResource(
        g_screenTex.Get(),
        frameTex.Get()
    );


    g_dupl->ReleaseFrame();

    return true;
}

// =====================================================================================
// GRADING PIPELINE
// =====================================================================================

static bool InitGradingPipeline()
{
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;


    if (!CompileShaderFromString(
        kVertexShaderSrc,
        "main",
        "vs_5_0",
        &vsBlob
    ))
    {
        return false;
    }


    if (!CompileShaderFromString(
        kPixelShaderSrc,
        "main",
        "ps_5_0",
        &psBlob
    ))
    {
        return false;
    }


    if (FAILED(
        g_device->CreateVertexShader(
            vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(),
            nullptr,
            &g_vs
        )
    ))
    {
        return false;
    }


    if (FAILED(
        g_device->CreatePixelShader(
            psBlob->GetBufferPointer(),
            psBlob->GetBufferSize(),
            nullptr,
            &g_ps
        )
    ))
    {
        return false;
    }


    D3D11_BUFFER_DESC cbDesc = {};

    cbDesc.ByteWidth =
        sizeof(GradeParams);

    cbDesc.Usage =
        D3D11_USAGE_DYNAMIC;

    cbDesc.BindFlags =
        D3D11_BIND_CONSTANT_BUFFER;

    cbDesc.CPUAccessFlags =
        D3D11_CPU_ACCESS_WRITE;


    if (FAILED(
        g_device->CreateBuffer(
            &cbDesc,
            nullptr,
            &g_paramsCB
        )
    ))
    {
        return false;
    }


    D3D11_SAMPLER_DESC sampDesc = {};

    sampDesc.Filter =
        D3D11_FILTER_MIN_MAG_MIP_LINEAR;

    sampDesc.AddressU =
        D3D11_TEXTURE_ADDRESS_CLAMP;

    sampDesc.AddressV =
        D3D11_TEXTURE_ADDRESS_CLAMP;

    sampDesc.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;


    if (FAILED(
        g_device->CreateSamplerState(
            &sampDesc,
            &g_sampler
        )
    ))
    {
        return false;
    }


    return true;
}

// =====================================================================================
// UPLOAD PARAMETERS
// =====================================================================================

static void UploadParams()
{
    GradeParams upload =
        g_params;

    upload.screenWidth =
        static_cast<float>(
            g_screenW
            );

    upload.screenHeight =
        static_cast<float>(
            g_screenH
            );


    D3D11_MAPPED_SUBRESOURCE mapped = {};


    HRESULT hr =
        g_context->Map(
            g_paramsCB.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped
        );


    if (FAILED(hr))
        return;


    memcpy(
        mapped.pData,
        &upload,
        sizeof(GradeParams)
    );


    g_context->Unmap(
        g_paramsCB.Get(),
        0
    );
}

// =====================================================================================
// RENDER GRADED SCREEN
// =====================================================================================

static void RenderGradedScreen()
{
    if (!g_screenSRV)
        return;


    D3D11_VIEWPORT vp = {};

    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;

    vp.Width =
        static_cast<float>(
            g_screenW
            );

    vp.Height =
        static_cast<float>(
            g_screenH
            );

    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;


    g_context->RSSetViewports(
        1,
        &vp
    );


    ID3D11RenderTargetView* rtv =
        g_rtv.Get();


    g_context->OMSetRenderTargets(
        1,
        &rtv,
        nullptr
    );


    UploadParams();


    g_context->VSSetShader(
        g_vs.Get(),
        nullptr,
        0
    );


    g_context->PSSetShader(
        g_ps.Get(),
        nullptr,
        0
    );


    ID3D11Buffer* cb =
        g_paramsCB.Get();


    g_context->PSSetConstantBuffers(
        0,
        1,
        &cb
    );


    ID3D11ShaderResourceView* srv =
        g_screenSRV.Get();


    g_context->PSSetShaderResources(
        0,
        1,
        &srv
    );


    ID3D11SamplerState* samp =
        g_sampler.Get();


    g_context->PSSetSamplers(
        0,
        1,
        &samp
    );


    g_context->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    );


    g_context->IASetInputLayout(
        nullptr
    );


    g_context->Draw(
        3,
        0
    );
}

// =====================================================================================
// UI STYLE
// =====================================================================================

static void ApplyCalmStyle()
{
    ImGuiStyle& s =
        ImGui::GetStyle();


    s.WindowRounding = 10.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 5.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 5.0f;
    s.ScrollbarRounding = 8.0f;


    s.WindowPadding =
        ImVec2(18, 16);

    s.FramePadding =
        ImVec2(10, 6);

    s.ItemSpacing =
        ImVec2(10, 12);

    s.ItemInnerSpacing =
        ImVec2(8, 6);


    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;

    s.GrabMinSize = 10.0f;


    ImVec4* c =
        s.Colors;


    c[ImGuiCol_WindowBg] =
        ImVec4(
            0.114f,
            0.125f,
            0.145f,
            0.96f
        );


    c[ImGuiCol_TitleBg] =
        ImVec4(
            0.114f,
            0.125f,
            0.145f,
            1.00f
        );


    c[ImGuiCol_TitleBgActive] =
        ImVec4(
            0.145f,
            0.157f,
            0.180f,
            1.00f
        );


    c[ImGuiCol_Text] =
        ImVec4(
            0.88f,
            0.89f,
            0.91f,
            1.00f
        );


    c[ImGuiCol_TextDisabled] =
        ImVec4(
            0.55f,
            0.57f,
            0.60f,
            1.00f
        );


    c[ImGuiCol_Separator] =
        ImVec4(
            1.0f,
            1.0f,
            1.0f,
            0.07f
        );


    c[ImGuiCol_Border] =
        ImVec4(
            1.0f,
            1.0f,
            1.0f,
            0.06f
        );


    c[ImGuiCol_ChildBg] =
        ImVec4(
            0.16f,
            0.175f,
            0.20f,
            1.00f
        );


    c[ImGuiCol_FrameBg] =
        ImVec4(
            0.18f,
            0.20f,
            0.23f,
            1.00f
        );


    c[ImGuiCol_FrameBgHovered] =
        ImVec4(
            0.22f,
            0.24f,
            0.28f,
            1.00f
        );


    c[ImGuiCol_FrameBgActive] =
        ImVec4(
            0.24f,
            0.27f,
            0.31f,
            1.00f
        );


    ImVec4 accent(
        0.42f,
        0.55f,
        0.62f,
        1.00f
    );


    ImVec4 accentHover(
        0.48f,
        0.62f,
        0.70f,
        1.00f
    );


    ImVec4 accentActive(
        0.38f,
        0.50f,
        0.57f,
        1.00f
    );


    c[ImGuiCol_SliderGrab] =
        accent;

    c[ImGuiCol_SliderGrabActive] =
        accentActive;


    c[ImGuiCol_Button] =
        ImVec4(
            0.20f,
            0.22f,
            0.26f,
            1.00f
        );


    c[ImGuiCol_ButtonHovered] =
        accentHover;


    c[ImGuiCol_ButtonActive] =
        accentActive;


    c[ImGuiCol_CheckMark] =
        accent;


    c[ImGuiCol_Header] =
        ImVec4(
            0.20f,
            0.22f,
            0.26f,
            1.00f
        );


    c[ImGuiCol_HeaderHovered] =
        accentHover;


    c[ImGuiCol_HeaderActive] =
        accentActive;
}

// =====================================================================================
// UI CARDS
// =====================================================================================

static void BeginCard(
    const char* id,
    const char* title,
    float height
)
{
    ImGui::TextColored(
        ImVec4(
            0.62f,
            0.68f,
            0.74f,
            1.0f
        ),
        "%s",
        title
    );


    ImGui::Spacing();


    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(14, 12)
    );


    ImGui::BeginChild(
        id,
        ImVec2(0, height),
        true,
        ImGuiWindowFlags_NoScrollbar
    );
}


static void EndCard()
{
    ImGui::EndChild();

    ImGui::PopStyleVar();

    ImGui::Dummy(
        ImVec2(0, 10)
    );
}

// =====================================================================================
// CONTROL PANEL
// =====================================================================================

static void DrawControlPanel()
{
    ImGui::SetNextWindowSize(
        ImVec2(390, 0),
        ImGuiCond_FirstUseEver
    );


    ImGui::Begin(
        "Color Grading",
        &g_panelVisible,
        ImGuiWindowFlags_NoCollapse
    );


    // ------------------------------------------------------------------------
    // HEADER
    // ------------------------------------------------------------------------

    ImGui::TextColored(
        ImVec4(
            0.55f,
            0.60f,
            0.65f,
            1.0f
        ),
        "INSERT  panel    SHIFT+M  exit"
    );


    ImGui::Dummy(
        ImVec2(0, 8)
    );


    // ------------------------------------------------------------------------
    // TONE
    // ------------------------------------------------------------------------

    BeginCard(
        "##tone",
        "TONE",
        150
    );


    ImGui::SliderFloat(
        "Brightness",
        &g_params.brightness,
        -0.5f,
        0.5f,
        "%.2f"
    );


    ImGui::SliderFloat(
        "Contrast",
        &g_params.contrast,
        0.0f,
        2.0f,
        "%.2f"
    );


    ImGui::SliderFloat(
        "Gamma",
        &g_params.gamma,
        0.2f,
        3.0f,
        "%.2f"
    );


    EndCard();


    // ------------------------------------------------------------------------
    // COLOR
    // ------------------------------------------------------------------------

    BeginCard(
        "##color",
        "COLOR",
        150
    );


    ImGui::SliderFloat(
        "Saturation",
        &g_params.saturation,
        0.0f,
        2.0f,
        "%.2f"
    );


    ImGui::SliderFloat(
        "Vibrance",
        &g_params.vibrance,
        -1.0f,
        1.0f,
        "%.2f"
    );


    ImGui::SliderFloat(
        "Temperature",
        &g_params.temperature,
        -1.0f,
        1.0f,
        "%.2f"
    );


    EndCard();


    // ------------------------------------------------------------------------
    // EFFECTS
    // ------------------------------------------------------------------------

    BeginCard(
        "##effects",
        "EFFECTS",
        150
    );


    ImGui::SliderFloat(
        "Bloom",
        &g_params.bloom,
        0.0f,
        1.0f,
        "%.2f"
    );


    ImGui::SliderFloat(
        "Clarity",
        &g_params.clarity,
        0.0f,
        2.0f,
        "%.2f"
    );


    ImGui::SliderFloat(
        "Sharpen",
        &g_params.sharpen,
        0.0f,
        2.0f,
        "%.2f"
    );


    EndCard();


    // ------------------------------------------------------------------------
    // RESET
    // ------------------------------------------------------------------------

    if (ImGui::Button(
        "Reset to neutral",
        ImVec2(-1, 0)
    ))
    {
        g_params =
            GradeParams{};

        g_params.screenWidth =
            static_cast<float>(
                g_screenW
                );

        g_params.screenHeight =
            static_cast<float>(
                g_screenH
                );
    }


    ImGui::Dummy(
        ImVec2(0, 4)
    );


    // ------------------------------------------------------------------------
    // PRESETS
    // ------------------------------------------------------------------------

    const float presetListHeight =
        std::max(
            60.0f,
            26.0f +
            ImGui::GetTextLineHeightWithSpacing() *
            static_cast<float>(
                std::min(
                    static_cast<int>(
                        g_presets.size()
                        ),
                    5
                )
                ) +
            8.0f
        );


    BeginCard(
        "##presets",
        "PRESETS",
        presetListHeight + 116.0f
    );


    // ------------------------------------------------------------------------
    // NAME INPUT
    // ------------------------------------------------------------------------

    ImGui::SetNextItemWidth(
        -90.0f
    );


    bool nameEdited =
        ImGui::InputTextWithHint(
            "##presetname",
            "preset name...",
            g_presetNameBuf,
            IM_ARRAYSIZE(
                g_presetNameBuf
            ),
            ImGuiInputTextFlags_EnterReturnsTrue
        );


    ImGui::SameLine();


    bool savePressed =
        ImGui::Button(
            "Save",
            ImVec2(80, 0)
        );


    if ((savePressed || nameEdited) &&
        g_presetNameBuf[0] != '\0')
    {
        SavePresetAs(
            std::string(
                g_presetNameBuf
            )
        );


        // Automatically select the saved preset.
        for (int i = 0;
            i < static_cast<int>(
                g_presets.size()
                );
            ++i)
        {
            if (g_presets[i].name ==
                g_presetNameBuf)
            {
                g_selectedPreset = i;
                break;
            }
        }


        g_presetNameBuf[0] =
            '\0';
    }


    ImGui::Dummy(
        ImVec2(0, 6)
    );


    // ------------------------------------------------------------------------
    // PRESET LIST
    // ------------------------------------------------------------------------

    ImGui::BeginChild(
        "##presetlist",
        ImVec2(
            0,
            presetListHeight
        ),
        false
    );


    for (int i = 0;
        i < static_cast<int>(
            g_presets.size()
            );
        ++i)
    {
        const bool isAutoLoad =
            (
                g_presets[i].name ==
                g_autoLoadName
                );


        std::string label;


        if (isAutoLoad)
        {
            label =
                "* " +
                g_presets[i].name;
        }
        else
        {
            label =
                "  " +
                g_presets[i].name;
        }


        if (ImGui::Selectable(
            label.c_str(),
            g_selectedPreset == i
        ))
        {
            g_selectedPreset =
                i;
        }
    }


    ImGui::EndChild();


    // ------------------------------------------------------------------------
    // PRESET BUTTONS
    // ------------------------------------------------------------------------

    const bool hasSelection =
        g_selectedPreset >= 0 &&
        g_selectedPreset <
        static_cast<int>(
            g_presets.size()
            );


    ImGui::BeginDisabled(
        !hasSelection
    );


    if (ImGui::Button(
        "Load",
        ImVec2(80, 0)
    ) &&
        hasSelection)
    {
        g_params =
            g_presets[
                g_selectedPreset
            ].params;
    }


    ImGui::SameLine();


    if (ImGui::Button(
        "Delete",
        ImVec2(80, 0)
    ) &&
        hasSelection)
    {
        DeletePreset(
            g_selectedPreset
        );
    }


    ImGui::SameLine();


    const bool isCurrentAutoLoad =
        hasSelection &&
        g_presets[
            g_selectedPreset
        ].name ==
        g_autoLoadName;


            if (ImGui::Button(
                isCurrentAutoLoad
                ? "Unset auto"
                : "Auto-load",
                ImVec2(-1, 0)
            ) &&
                hasSelection)
            {
                if (isCurrentAutoLoad)
                {
                    g_autoLoadName.clear();
                }
                else
                {
                    g_autoLoadName =
                        g_presets[
                            g_selectedPreset
                        ].name;
                }


                SavePresets();
            }


            ImGui::EndDisabled();


            EndCard();


            ImGui::End();
}

// =====================================================================================
// WINDOW PROCEDURE
// =====================================================================================

LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (ImGui_ImplWin32_WndProcHandler(
        hwnd,
        msg,
        wParam,
        lParam
    ))
    {
        return true;
    }


    switch (msg)
    {
    case WM_HOTKEY:

        if (wParam ==
            HOTKEY_TOGGLE_PANEL)
        {
            g_panelVisible =
                !g_panelVisible;


            SetClickThrough(
                !g_panelVisible
            );


            if (g_panelVisible)
            {
                ShowWindow(
                    g_hwnd,
                    SW_SHOW
                );


                // Make the window keyboard
                // interactive while the
                // control panel is open.
                LONG_PTR exStyle =
                    GetWindowLongPtr(
                        g_hwnd,
                        GWL_EXSTYLE
                    );


                exStyle &=
                    ~WS_EX_NOACTIVATE;


                SetWindowLongPtr(
                    g_hwnd,
                    GWL_EXSTYLE,
                    exStyle
                );


                SetWindowPos(
                    g_hwnd,
                    HWND_TOPMOST,
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE |
                    SWP_NOSIZE |
                    SWP_SHOWWINDOW
                );


                SetForegroundWindow(
                    g_hwnd
                );


                SetFocus(
                    g_hwnd
                );
            }
            else
            {
                SetClickThrough(
                    true
                );


                SetWindowPos(
                    g_hwnd,
                    HWND_TOPMOST,
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE |
                    SWP_NOSIZE |
                    SWP_NOACTIVATE
                );
            }


            return 0;
        }


        if (wParam ==
            HOTKEY_KILLSWITCH)
        {
            KillApplication();

            return 0;
        }

        break;


    case WM_MOUSEACTIVATE:

        if (g_panelVisible &&
            !g_clickThrough)
        {
            return MA_ACTIVATE;
        }

        return MA_NOACTIVATE;


    case WM_CLOSE:

        DestroyWindow(
            hwnd
        );

        return 0;


    case WM_DESTROY:

        PostQuitMessage(
            0
        );

        return 0;
    }


    return DefWindowProc(
        hwnd,
        msg,
        wParam,
        lParam
    );
}

// =====================================================================================
// ENTRY POINT
// =====================================================================================

int WINAPI WinMain(
    HINSTANCE hInst,
    HINSTANCE,
    LPSTR,
    int
)
{
    // ------------------------------------------------------------------------
    // Window class
    // ------------------------------------------------------------------------

    WNDCLASSEX wc =
    {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0,
        0,
        hInst,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        L"OverlayGradeWnd",
        nullptr
    };


    RegisterClassEx(
        &wc
    );


    // ------------------------------------------------------------------------
    // Primary monitor dimensions
    // ------------------------------------------------------------------------

    const int screenW =
        GetSystemMetrics(
            SM_CXSCREEN
        );


    const int screenH =
        GetSystemMetrics(
            SM_CYSCREEN
        );


    // ------------------------------------------------------------------------
    // Create overlay
    //
    // IMPORTANT:
    // No WS_EX_NOACTIVATE here.
    // That allows ImGui InputText to properly
    // receive keyboard focus.
    // ------------------------------------------------------------------------

    g_hwnd =
        CreateWindowEx(
            WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW |
            WS_EX_LAYERED,

            wc.lpszClassName,

            L"Screen Grading Overlay",

            WS_POPUP,

            0,
            0,
            screenW,
            screenH,

            nullptr,
            nullptr,
            hInst,
            nullptr
        );


    if (!g_hwnd)
    {
        MessageBoxW(
            nullptr,
            L"Failed to create overlay window.",
            L"Screen Grading Overlay",
            MB_OK |
            MB_ICONERROR
        );

        return 1;
    }


    // ------------------------------------------------------------------------
    // Layered window
    // ------------------------------------------------------------------------

    SetLayeredWindowAttributes(
        g_hwnd,
        0,
        255,
        LWA_ALPHA
    );


    // ------------------------------------------------------------------------
    // Prevent overlay from being
    // captured by Desktop Duplication
    // ------------------------------------------------------------------------

    SetWindowDisplayAffinity(
        g_hwnd,
        WDA_EXCLUDEFROMCAPTURE
    );


    // ------------------------------------------------------------------------
    // Global hotkeys
    // ------------------------------------------------------------------------

    RegisterHotKey(
        g_hwnd,
        HOTKEY_TOGGLE_PANEL,
        MOD_NOREPEAT,
        VK_INSERT
    );


    RegisterHotKey(
        g_hwnd,
        HOTKEY_KILLSWITCH,
        MOD_SHIFT |
        MOD_NOREPEAT,
        'M'
    );


    // ------------------------------------------------------------------------
    // Show
    // ------------------------------------------------------------------------

    ShowWindow(
        g_hwnd,
        SW_SHOW
    );


    UpdateWindow(
        g_hwnd
    );


    // ------------------------------------------------------------------------
    // D3D
    // ------------------------------------------------------------------------

    if (!InitD3D(
        g_hwnd
    ))
    {
        MessageBoxW(
            nullptr,
            L"D3D initialization failed.",
            L"Screen Grading Overlay",
            MB_OK |
            MB_ICONERROR
        );

        DestroyWindow(
            g_hwnd
        );

        return 1;
    }


    // ------------------------------------------------------------------------
    // Desktop duplication
    // ------------------------------------------------------------------------

    if (!InitDesktopDuplication())
    {
        MessageBoxW(
            nullptr,
            L"Desktop Duplication initialization failed.\n\n"
            L"Check your GPU driver and display configuration.",
            L"Screen Grading Overlay",
            MB_OK |
            MB_ICONERROR
        );

        DestroyWindow(
            g_hwnd
        );

        return 1;
    }


    // ------------------------------------------------------------------------
    // Shader pipeline
    // ------------------------------------------------------------------------

    if (!InitGradingPipeline())
    {
        MessageBoxW(
            nullptr,
            L"Shader initialization failed.",
            L"Screen Grading Overlay",
            MB_OK |
            MB_ICONERROR
        );

        DestroyWindow(
            g_hwnd
        );

        return 1;
    }


    // ------------------------------------------------------------------------
    // Config
    // ------------------------------------------------------------------------

    LoadPresets();

    ApplyAutoLoadIfAny();


    // ------------------------------------------------------------------------
    // ImGui
    // ------------------------------------------------------------------------

    IMGUI_CHECKVERSION();

    ImGui::CreateContext();


    ImGuiIO& io =
        ImGui::GetIO();

    (void)io;


    ImGui::StyleColorsDark();

    ApplyCalmStyle();


    if (!ImGui_ImplWin32_Init(
        g_hwnd
    ))
    {
        MessageBoxW(
            nullptr,
            L"ImGui Win32 initialization failed.",
            L"Screen Grading Overlay",
            MB_OK |
            MB_ICONERROR
        );

        return 1;
    }


    if (!ImGui_ImplDX11_Init(
        g_device.Get(),
        g_context.Get()
    ))
    {
        MessageBoxW(
            nullptr,
            L"ImGui DX11 initialization failed.",
            L"Screen Grading Overlay",
            MB_OK |
            MB_ICONERROR
        );

        return 1;
    }


    // ------------------------------------------------------------------------
    // Main loop
    // ------------------------------------------------------------------------

    bool running = true;


    while (running)
    {
        MSG msg;


        while (PeekMessage(
            &msg,
            nullptr,
            0,
            0,
            PM_REMOVE
        ))
        {
            if (msg.message ==
                WM_QUIT)
            {
                running = false;
            }


            TranslateMessage(
                &msg
            );


            DispatchMessage(
                &msg
            );
        }


        if (!running)
            break;


        // ------------------------------------------------------------
        // Capture desktop
        // ------------------------------------------------------------

        CaptureFrame();


        // ------------------------------------------------------------
        // ImGui frame
        // ------------------------------------------------------------

        ImGui_ImplDX11_NewFrame();

        ImGui_ImplWin32_NewFrame();

        ImGui::NewFrame();


        // ------------------------------------------------------------
        // Graded desktop
        // ------------------------------------------------------------

        RenderGradedScreen();


        // ------------------------------------------------------------
        // UI
        // ------------------------------------------------------------

        if (g_panelVisible)
        {
            DrawControlPanel();
        }


        // ------------------------------------------------------------
        // Render ImGui
        // ------------------------------------------------------------

        ImGui::Render();


        ImGui_ImplDX11_RenderDrawData(
            ImGui::GetDrawData()
        );


        // ------------------------------------------------------------
        // Present
        // ------------------------------------------------------------

        HRESULT presentResult =
            g_swapChain->Present(
                1,
                0
            );


        if (presentResult ==
            DXGI_ERROR_DEVICE_REMOVED ||
            presentResult ==
            DXGI_ERROR_DEVICE_RESET)
        {
            running = false;
        }
    }


    // =================================================================================
    // CLEANUP
    // =================================================================================

    UnregisterHotKey(
        g_hwnd,
        HOTKEY_TOGGLE_PANEL
    );


    UnregisterHotKey(
        g_hwnd,
        HOTKEY_KILLSWITCH
    );


    ImGui_ImplDX11_Shutdown();

    ImGui_ImplWin32_Shutdown();

    ImGui::DestroyContext();


    g_screenSRV.Reset();
    g_screenTex.Reset();
    g_dupl.Reset();

    g_paramsCB.Reset();
    g_sampler.Reset();

    g_vs.Reset();
    g_ps.Reset();

    g_rtv.Reset();
    g_swapChain.Reset();
    g_context.Reset();
    g_device.Reset();


    if (g_hwnd)
    {
        DestroyWindow(
            g_hwnd
        );
    }


    UnregisterClass(
        wc.lpszClassName,
        hInst
    );


    return 0;
}
