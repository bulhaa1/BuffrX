// Present-loop load harness: the honest substitute for a game when the machine has none.
//
// The in-game overhead figure in docs/measurement.md needs a game running. This is not a game: it is a
// window that presents as fast as the GPU lets it and reports its own frame rate, so the same
// measurement (average and 1% low, capture off vs on) can be taken with a workload that is identical
// every run. Results are labelled with this fact wherever they are published.
//
//   buffrx-present-loop.exe --seconds 60 [--width 1280] [--height 720] [--vsync 1] [--fullscreen 1]
//                           [--probe 1] [--motion 1] [--cap 144] [--load 600] [--settle 10]
//
// --settle S leaves the first S seconds out of the reported average and 1% low, so a run that starts something (an
// engine, a capture) measures only the steady state.
//
// --load N turns the loop into a GPU-bound stand-in for a game: every frame draws one full-window triangle whose
// pixel shader runs N*16 dependent sin/cos steps per pixel, so the frame rate is set by how fast the GPU gets through
// the shader, not by Present. Without it the loop is not GPU-bound at all (15 841 fps uncapped in
// docs/measurement.md), and a cost the engine puts on the GPU cannot show up in its frame rate. It replaces the
// other drawing modes. Pick N so the unloaded frame rate lands where a game's does (100-160 fps).
//
// --probe stamps each presented frame's number into a row of black and white blocks in the top-left
// corner (28 bits, then a fixed guard), so `buffrx-engine.exe --media-sequence <recording>` can read back
// which source frames a recording holds: the ones it skipped and the ones it repeated, exactly. --cap
// --motion draws several hundred coloured rectangles that move a little every frame over a shifting
// background: high-detail, temporally coherent content, which is what makes an encoder work. The flat
// colour of the plain loop is too easy for any encoder setting to be told apart by. --cap
// limits the presentation rate the way a game's frame limiter does, so the capture can be tested
// against a source that is not at the display's rate.
//
// The window is also a capture target for the engine: window capture of it is the "buffering on" case.
#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool has_argument(int argc, char** argv, const char* name, int& out) {
  for (int i = 1; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], name) == 0) {
      out = std::atoi(argv[i + 1]);
      return true;
    }
  }
  return false;
}

HWND g_window = nullptr;
bool g_closed = false;
bool g_minimize_on_blur = false;
bool g_had_focus = false;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_ACTIVATEAPP) {
    if (wparam) g_had_focus = true;
    else if (g_minimize_on_blur && g_had_focus) ShowWindow(window, SW_MINIMIZE);
  }
  if (message == WM_CLOSE || message == WM_DESTROY) {
    g_closed = true;
    return 0;
  }
  if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
    g_closed = true;
    return 0;
  }
  return DefWindowProcA(window, message, wparam, lparam);
}

double now_seconds() {
  LARGE_INTEGER counter{}, frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = 60;
  int width = 1280;
  int height = 720;
  int vsync = 0;
  has_argument(argc, argv, "--seconds", seconds);
  has_argument(argc, argv, "--width", width);
  has_argument(argc, argv, "--height", height);
  has_argument(argc, argv, "--vsync", vsync);
  // --fullscreen 1: a borderless window covering the primary monitor, taken to the foreground, the way
  // most games run today. It stands in for a game when game detection and game capture are tested.
  int fullscreen = 0;
  has_argument(argc, argv, "--fullscreen", fullscreen);
  int exclusive = 0;
  has_argument(argc, argv, "--exclusive", exclusive);
  int minimize_on_blur = 0;
  has_argument(argc, argv, "--minimize-on-blur", minimize_on_blur);
  g_minimize_on_blur = minimize_on_blur != 0;
  if (exclusive) fullscreen = 1;
  if (fullscreen) {
    width = GetSystemMetrics(SM_CXSCREEN);
    height = GetSystemMetrics(SM_CYSCREEN);
  }
  if (seconds < 5) seconds = 5;
  int probe = 0, cap = 0, motion = 0, load = 0, settle = 0;
  has_argument(argc, argv, "--settle", settle);
  has_argument(argc, argv, "--load", load);
  has_argument(argc, argv, "--probe", probe);
  has_argument(argc, argv, "--motion", motion);
  has_argument(argc, argv, "--cap", cap);

  WNDCLASSEXA cls{};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = &window_proc;
  cls.hInstance = GetModuleHandleA(nullptr);
  cls.lpszClassName = "BuffrX.PresentLoop";
  cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassExA(&cls);
  // Topmost when it covers the screen: a probe the capture cannot see measures nothing.
  g_window = CreateWindowExA(fullscreen ? WS_EX_TOPMOST : 0, cls.lpszClassName, "BuffrX present loop", fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW,
                             fullscreen ? 0 : 60, fullscreen ? 0 : 60, width, height,
                             nullptr, nullptr, cls.hInstance, nullptr);
  if (!g_window) {
    std::printf("PRESENT_LOOP_FAIL window\n");
    return 1;
  }
  ShowWindow(g_window, SW_SHOW);
  if (fullscreen) {
    // Windows refuses SetForegroundWindow to a process started in the background; a synthetic Alt
    // press is the documented way an application regains the right to take the foreground.
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(g_window);
  }

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL level{};
  const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted, 2, D3D11_SDK_VERSION, &device,
                                 &level, &context);
  if (FAILED(hr)) {
    std::printf("PRESENT_LOOP_FAIL d3d11 0x%08lX\n", static_cast<unsigned long>(hr));
    return 1;
  }

  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory2* factory = nullptr;
  if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) ||
      FAILED(dxgi_device->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) {
    std::printf("PRESENT_LOOP_FAIL dxgi\n");
    return 1;
  }

  DXGI_SWAP_CHAIN_DESC1 swap_desc{};
  swap_desc.Width = static_cast<UINT>(width);
  swap_desc.Height = static_cast<UINT>(height);
  swap_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  swap_desc.SampleDesc.Count = 1;
  swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_desc.BufferCount = 2;
  swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1* swap = nullptr;
  hr = factory->CreateSwapChainForHwnd(device, g_window, &swap_desc, nullptr, nullptr, &swap);
  if (FAILED(hr)) {
    std::printf("PRESENT_LOOP_FAIL swapchain 0x%08lX\n", static_cast<unsigned long>(hr));
    return 1;
  }
  if (exclusive) {
    hr = swap->SetFullscreenState(TRUE, nullptr);
    if (FAILED(hr)) {
      std::printf("PRESENT_LOOP_FAIL exclusive 0x%08lX\n", static_cast<unsigned long>(hr));
      return 1;
    }
    hr = swap->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      std::printf("PRESENT_LOOP_FAIL exclusive resize 0x%08lX\n", static_cast<unsigned long>(hr));
      swap->SetFullscreenState(FALSE, nullptr);
      return 1;
    }
    BOOL active = FALSE;
    swap->GetFullscreenState(&active, nullptr);
    std::printf("PRESENT_LOOP_EXCLUSIVE active=%d\n", active ? 1 : 0);
    std::fflush(stdout);
  }

  ID3D11Texture2D* back_buffer = nullptr;
  ID3D11RenderTargetView* target = nullptr;
  if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer))) ||
      FAILED(device->CreateRenderTargetView(back_buffer, nullptr, &target))) {
    std::printf("PRESENT_LOOP_FAIL render target\n");
    return 1;
  }

  struct Block {
    float x, y, w, h, vx, vy;
    float color[4];
  };
  std::vector<Block> blocks;
  if (motion) {
    unsigned seed = 12345;
    auto random = [&seed]() {
      seed = seed * 1664525u + 1013904223u;
      return static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f;
    };
    for (int i = 0; i < 700; ++i) {
      Block block{};
      block.w = 12.0f + random() * 110.0f;
      block.h = 12.0f + random() * 110.0f;
      block.x = random() * static_cast<float>(width);
      block.y = random() * static_cast<float>(height);
      block.vx = (random() - 0.5f) * 14.0f;
      block.vy = (random() - 0.5f) * 14.0f;
      block.color[0] = random();
      block.color[1] = random();
      block.color[2] = random();
      block.color[3] = 1.0f;
      blocks.push_back(block);
    }
  }
  // The GPU load: a fullscreen triangle from SV_VertexID and a pixel shader with a dependent chain of N*16 steps. The
  // compiler is loaded at run time, so the tool links nothing new.
  ID3D11VertexShader* load_vs = nullptr;
  ID3D11PixelShader* load_ps = nullptr;
  ID3D11Buffer* load_constants = nullptr;
  if (load > 0) {
    typedef HRESULT(WINAPI * CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                         UINT, ID3DBlob**, ID3DBlob**);
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    CompileFn compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) {
      std::printf("PRESENT_LOOP_FAIL d3dcompiler\n");
      return 1;
    }
    const std::string source =
        "cbuffer C : register(b0) { float phase; float3 pad; };\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
        "  float2 p = float2(id & 2, (id << 1) & 2);\n"
        "  return float4(p * 2.0 - 1.0, 0, 1);\n"
        "}\n"
        "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
        "  float2 uv = pos.xy * 0.001;\n"
        "  float v = phase + uv.x;\n"
        "  [loop] for (int i = 0; i < " + std::to_string(load * 16) + "; ++i) {\n"
        "    v = sin(v * 1.7 + uv.y) * 0.5 + cos(v * 0.9 + uv.x) * 0.5 + 0.1;\n"
        "  }\n"
        "  float t = phase * 62.83;\n"
        "  return float4(0.5 + 0.5 * sin(uv.x * 40.0 + t + v), 0.5 + 0.5 * sin(uv.y * 50.0 + t * 1.3 + v * 2.0), frac(v * 3.0), 1.0);\n"
        "}\n";
    ID3DBlob *vs_blob = nullptr, *ps_blob = nullptr, *errors = nullptr;
    if (FAILED(compile(source.data(), source.size(), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs_blob, &errors)) ||
        FAILED(compile(source.data(), source.size(), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps_blob, &errors)) ||
        FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &load_vs)) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &load_ps))) {
      std::printf("PRESENT_LOOP_FAIL load shader %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
      return 1;
    }
    vs_blob->Release();
    ps_blob->Release();
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 16;
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&constants, nullptr, &load_constants);
  }
  // The GPU's own time for the load pass, from timestamp queries read a few frames late: the frame rate alone cannot say
  // whether the load is heavy when the window is presented at the display's rate.
  struct GpuQuery {
    ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr;
  };
  GpuQuery gpu_queries[4];
  int gpu_slot = 0, gpu_issued = 0;
  double gpu_ms_total = 0.0;
  int gpu_ms_count = 0;
  if (load > 0) {
    for (GpuQuery& q : gpu_queries) {
      D3D11_QUERY_DESC d{};
      d.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
      device->CreateQuery(&d, &q.disjoint);
      d.Query = D3D11_QUERY_TIMESTAMP;
      device->CreateQuery(&d, &q.begin);
      device->CreateQuery(&d, &q.end);
    }
  }
  ID3D11DeviceContext1* context1 = nullptr;
  if (probe || motion) context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1));
  const int cell = std::max(4, height / 36), margin = std::max(2, height / 180), pitch = cell + std::max(1, height / 360);

  std::vector<double> frame_times;
  frame_times.reserve(static_cast<size_t>(seconds) * 600);
  const double start = now_seconds();
  double previous = start;
  int frames = 0;
  int settled_frames = 0;
  bool was_exclusive = exclusive != 0;
  bool reported_present_failure = false;
  double settled_start = start;
  while (!g_closed) {
    MSG message;
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageA(&message);
    }
    if (g_closed) break;
    if (exclusive) {
      BOOL active = FALSE;
      swap->GetFullscreenState(&active, nullptr);
      if (was_exclusive && !active) {
        std::printf("PRESENT_LOOP_EXCLUSIVE_LOST iconic=%d foreground=%d\n", IsIconic(g_window) ? 1 : 0,
                    GetForegroundWindow() == g_window ? 1 : 0);
        std::fflush(stdout);
        was_exclusive = false;
      }
    }

    const double now = now_seconds();
    if (now - start >= seconds) break;

    if (load > 0) {
      GpuQuery& q = gpu_queries[gpu_slot];
      if (q.disjoint) {
        context->Begin(q.disjoint);
        context->End(q.begin);
      }
      const float constants[4] = {static_cast<float>(frames % 600) / 600.0f, 0, 0, 0};
      context->UpdateSubresource(load_constants, 0, nullptr, constants, 0, 0);
      const D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
      context->RSSetViewports(1, &viewport);
      context->OMSetRenderTargets(1, &target, nullptr);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->VSSetShader(load_vs, nullptr, 0);
      context->PSSetShader(load_ps, nullptr, 0);
      context->PSSetConstantBuffers(0, 1, &load_constants);
      context->Draw(3, 0);
      if (q.disjoint) {
        context->End(q.end);
        context->End(q.disjoint);
        gpu_slot = (gpu_slot + 1) % 4;
        ++gpu_issued;
        // The oldest slot is read once it has had three frames to finish.
        if (gpu_issued >= 4) {
          GpuQuery& old = gpu_queries[gpu_slot];
          D3D11_QUERY_DATA_TIMESTAMP_DISJOINT info{};
          UINT64 t0 = 0, t1 = 0;
          if (context->GetData(old.disjoint, &info, sizeof(info), 0) == S_OK && !info.Disjoint &&
              context->GetData(old.begin, &t0, sizeof(t0), 0) == S_OK && context->GetData(old.end, &t1, sizeof(t1), 0) == S_OK &&
              info.Frequency > 0) {
            gpu_ms_total += static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(info.Frequency);
            ++gpu_ms_count;
          }
        }
      }
    } else if (motion && context1) {
      const float phase = static_cast<float>(frames % 600) / 600.0f;
      const float background[4] = {0.15f + 0.2f * phase, 0.2f, 0.35f - 0.2f * phase, 1.0f};
      context->ClearRenderTargetView(target, background);
      for (Block& block : blocks) {
        block.x += block.vx;
        block.y += block.vy;
        if (block.x < -block.w) block.x += static_cast<float>(width) + block.w;
        if (block.x > static_cast<float>(width)) block.x -= static_cast<float>(width) + block.w;
        if (block.y < -block.h) block.y += static_cast<float>(height) + block.h;
        if (block.y > static_cast<float>(height)) block.y -= static_cast<float>(height) + block.h;
        const D3D11_RECT rect = {static_cast<LONG>(std::max(0.0f, block.x)), static_cast<LONG>(std::max(0.0f, block.y)),
                                 static_cast<LONG>(std::min(static_cast<float>(width), block.x + block.w)),
                                 static_cast<LONG>(std::min(static_cast<float>(height), block.y + block.h))};
        if (rect.right > rect.left && rect.bottom > rect.top) context1->ClearView(target, block.color, &rect, 1);
      }
    } else if (probe && context1) {
      const float gray[4] = {0.25f, 0.25f, 0.25f, 1.0f}, white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1};
      context->ClearRenderTargetView(target, gray);
      std::vector<D3D11_RECT> ones, zeros;
      for (int bit = 0; bit < 32; ++bit) {
        // 28 counter bits, then the fixed guard 1010 so a misread frame is recognised.
        const bool set = bit < 28 ? ((frames >> bit) & 1) != 0 : (((0xA >> (bit - 28)) & 1) != 0);
        const D3D11_RECT rect = {margin + bit * pitch, margin, margin + bit * pitch + cell, margin + cell};
        (set ? ones : zeros).push_back(rect);
      }
      if (!ones.empty()) context1->ClearView(target, white, ones.data(), static_cast<UINT>(ones.size()));
      if (!zeros.empty()) context1->ClearView(target, black, zeros.data(), static_cast<UINT>(zeros.size()));
    } else {
      const float phase = static_cast<float>(frames % 240) / 240.0f;
      const float color[4] = {phase, 1.0f - phase, 0.5f, 1.0f};
      context->ClearRenderTargetView(target, color);
    }
    const HRESULT present_result = swap->Present(vsync ? 1 : 0, 0);
    if (exclusive && FAILED(present_result) && !reported_present_failure) {
      std::printf("PRESENT_LOOP_PRESENT_FAIL 0x%08lX\n", static_cast<unsigned long>(present_result));
      std::fflush(stdout);
      reported_present_failure = true;
    }
    ++frames;
    if (cap > 0) {
      // A frame limiter: wait for the next slot on the high-resolution clock, as a game's own does.
      const double slot = start + static_cast<double>(frames) / cap;
      while (now_seconds() < slot) {
        if (slot - now_seconds() > 0.002) Sleep(1);
      }
    }
    if (now - start >= settle) {
      if (settled_frames == 0) settled_start = now;
      ++settled_frames;
      frame_times.push_back((now - previous) * 1000.0);
    }
    previous = now;
  }

  const double elapsed = now_seconds() - start;
  std::vector<double> sorted = frame_times;
  std::sort(sorted.begin(), sorted.end());
  const size_t p99_index = sorted.empty() ? 0 : static_cast<size_t>(sorted.size() * 0.99);
  const double p99_ms = sorted.empty() ? 0.0 : sorted[std::min(p99_index, sorted.size() - 1)];
  const double settled_elapsed = now_seconds() - settled_start;
  const double fps_avg = settled_elapsed > 0.0 && settled_frames > 0 ? settled_frames / settled_elapsed : (elapsed > 0.0 ? frames / elapsed : 0.0);
  const double fps_low = p99_ms > 0.0 ? 1000.0 / p99_ms : 0.0;
  std::printf("PRESENT_LOOP seconds=%.1f frames=%d fps_avg=%.2f frame_p99_ms=%.3f fps_1pct_low=%.2f vsync=%d", elapsed,
              frames, fps_avg, p99_ms, fps_low, vsync);
  if (load > 0 && gpu_ms_count > 0) std::printf(" load=%d gpu_ms=%.3f", load, gpu_ms_total / gpu_ms_count);
  else if (load > 0) std::printf(" load=%d gpu_ms=n/a", load);
  std::printf("\n");

  if (load_vs) load_vs->Release();
  if (load_ps) load_ps->Release();
  if (load_constants) load_constants->Release();
  if (context1) context1->Release();
  target->Release();
  back_buffer->Release();
  if (exclusive) swap->SetFullscreenState(FALSE, nullptr);
  swap->Release();
  factory->Release();
  adapter->Release();
  dxgi_device->Release();
  context->Release();
  device->Release();
  DestroyWindow(g_window);
  return 0;
}
