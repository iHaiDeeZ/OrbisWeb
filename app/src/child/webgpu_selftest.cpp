// Wisp: the WebGPU self-test of the GPU web process (wisp-web-gpu.bin), OrbisWeb's first step to
// WebGPU (docs/WEBGPU.md). Dawn (scripts/build-dawn.sh), its Vulkan backend on the kit's RADV, in the
// process that already has Zink's Vulkan device: whether a second device works beside it is the first
// question. Started when the UI asks for it (menu: WebGPU test; WISP_CHILD_WEBGPU_SELFTEST in the start
// message), a few seconds in, on a thread of its own. Each step is appended to
// /data/Wisp/run/webgpu-test.txt as it ends (a crash still leaves the steps before it) and to web.log:
//   - the adapter: its name, the driver, limits and features;
//   - a device;
//   - a WGSL compute shader writing 256 values, read back and checked;
//   - a triangle rendered into a 64x64 texture, read back, a pixel inside and one outside checked.
#include "../common/wisp.h"

#include <dawn/webgpu_cpp.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char *RESULT_FILE = WISP_DATA "/run/webgpu-test.txt";
using Clock = std::chrono::steady_clock;
Clock::time_point g_start;

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// a line to the result file (opened and closed each time: the lines before a crash stay) and web.log
void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
void say(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    wisp_log("webgpu test: %s", line);
    if (FILE *f = fopen(RESULT_FILE, "a")) {
        fprintf(f, "[%7.0f ms] %s\n", ms_since(g_start), line);
        fclose(f);
    }
}

std::string text(wgpu::StringView view) { return std::string(std::string_view(view)); }

const char *backend_name(wgpu::BackendType type)
{
    switch (type) {
    case wgpu::BackendType::Vulkan: return "Vulkan";
    case wgpu::BackendType::Null: return "null";
    default: return "other";
    }
}

const char *COMPUTE_WGSL = R"(
@group(0) @binding(0) var<storage, read_write> data : array<u32>;
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id : vec3u) {
    data[id.x] = id.x * 2u + 1u;
}
)";

const char *RENDER_WGSL = R"(
@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {
    var p = array<vec2f, 3>(vec2f(0.0, 0.8), vec2f(-0.8, -0.8), vec2f(0.8, -0.8));
    return vec4f(p[i], 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4f {
    return vec4f(1.0, 0.0, 0.0, 1.0);
}
)";

wgpu::ShaderModule shader(const wgpu::Device &device, const char *code)
{
    wgpu::ShaderSourceWGSL source = {};
    source.code = code;
    wgpu::ShaderModuleDescriptor descriptor = {};
    descriptor.nextInChain = &source;
    return device.CreateShaderModule(&descriptor);
}

// Maps buffer for reading and waits; false (said why) when it failed
bool map_read(const wgpu::Instance &instance, const wgpu::Buffer &buffer, size_t size)
{
    bool ok = false;
    std::string message;
    wgpu::Future future = buffer.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView why) {
            ok = status == wgpu::MapAsyncStatus::Success;
            message = text(why);
        });
    if (instance.WaitAny(future, 10'000'000'000ull) != wgpu::WaitStatus::Success) {
        say("FAIL: mapping a buffer did not end in 10 s");
        return false;
    }
    if (!ok)
        say("FAIL: mapping a buffer: %s", message.c_str());
    return ok;
}

bool compute_test(const wgpu::Instance &instance, const wgpu::Device &device)
{
    const Clock::time_point t = Clock::now();
    constexpr uint32_t N = 256;
    wgpu::ComputePipelineDescriptor pipeline_descriptor = {};
    pipeline_descriptor.compute.module = shader(device, COMPUTE_WGSL);
    pipeline_descriptor.compute.entryPoint = "main";
    wgpu::ComputePipeline pipeline = device.CreateComputePipeline(&pipeline_descriptor);
    say("compute: shader and pipeline in %.0f ms (WGSL to SPIR-V by Tint, then RADV's compiler)", ms_since(t));

    wgpu::BufferDescriptor storage_descriptor = {};
    storage_descriptor.size = N * 4;
    storage_descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
    wgpu::Buffer storage = device.CreateBuffer(&storage_descriptor);
    wgpu::BufferDescriptor readback_descriptor = {};
    readback_descriptor.size = N * 4;
    readback_descriptor.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readback = device.CreateBuffer(&readback_descriptor);

    wgpu::BindGroupEntry entry = {};
    entry.binding = 0;
    entry.buffer = storage;
    entry.size = N * 4;
    wgpu::BindGroupDescriptor group_descriptor = {};
    group_descriptor.layout = pipeline.GetBindGroupLayout(0);
    group_descriptor.entryCount = 1;
    group_descriptor.entries = &entry;
    wgpu::BindGroup group = device.CreateBindGroup(&group_descriptor);

    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, group);
    pass.DispatchWorkgroups(N / 64);
    pass.End();
    encoder.CopyBufferToBuffer(storage, 0, readback, 0, N * 4);
    wgpu::CommandBuffer commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);
    if (!map_read(instance, readback, N * 4))
        return false;
    const auto *values = static_cast<const uint32_t *>(readback.GetConstMappedRange(0, N * 4));
    uint32_t wrong = 0, first_wrong = N;
    for (uint32_t i = 0; values && i < N; i++)
        if (values[i] != i * 2 + 1 && wrong++ == 0)
            first_wrong = i;
    const bool ok = values && !wrong;
    if (ok)
        say("compute: PASS, 256 values right (%.0f ms in all)", ms_since(t));
    else if (!values)
        say("FAIL compute: no mapped memory");
    else
        say("FAIL compute: %u of 256 values wrong, the first at %u (%u, not %u)", wrong, first_wrong, values[first_wrong], first_wrong * 2 + 1);
    readback.Unmap();
    return ok;
}

bool render_test(const wgpu::Instance &instance, const wgpu::Device &device)
{
    const Clock::time_point t = Clock::now();
    constexpr uint32_t SIZE = 64, ROW = SIZE * 4;   // (a row is 256 bytes: WebGPU's copy alignment)
    wgpu::TextureDescriptor texture_descriptor = {};
    texture_descriptor.size = {SIZE, SIZE, 1};
    texture_descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
    texture_descriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    wgpu::Texture texture = device.CreateTexture(&texture_descriptor);

    wgpu::ShaderModule module = shader(device, RENDER_WGSL);
    wgpu::ColorTargetState target = {};
    target.format = wgpu::TextureFormat::RGBA8Unorm;
    wgpu::FragmentState fragment = {};
    fragment.module = module;
    fragment.entryPoint = "fs";
    fragment.targetCount = 1;
    fragment.targets = &target;
    wgpu::RenderPipelineDescriptor pipeline_descriptor = {};
    pipeline_descriptor.vertex.module = module;
    pipeline_descriptor.vertex.entryPoint = "vs";
    pipeline_descriptor.fragment = &fragment;
    wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&pipeline_descriptor);
    say("render: shaders and pipeline in %.0f ms", ms_since(t));

    wgpu::RenderPassColorAttachment attachment = {};
    attachment.view = texture.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = {0.0, 0.0, 0.0, 1.0};
    wgpu::RenderPassDescriptor pass_descriptor = {};
    pass_descriptor.colorAttachmentCount = 1;
    pass_descriptor.colorAttachments = &attachment;

    wgpu::BufferDescriptor readback_descriptor = {};
    readback_descriptor.size = ROW * SIZE;
    readback_descriptor.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readback = device.CreateBuffer(&readback_descriptor);

    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_descriptor);
    pass.SetPipeline(pipeline);
    pass.Draw(3);
    pass.End();
    wgpu::TexelCopyTextureInfo source = {};
    source.texture = texture;
    wgpu::TexelCopyBufferInfo destination = {};
    destination.buffer = readback;
    destination.layout.bytesPerRow = ROW;
    destination.layout.rowsPerImage = SIZE;
    wgpu::Extent3D copy_size = {SIZE, SIZE, 1};
    encoder.CopyTextureToBuffer(&source, &destination, &copy_size);
    wgpu::CommandBuffer commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);
    if (!map_read(instance, readback, ROW * SIZE))
        return false;
    const auto *pixels = static_cast<const uint8_t *>(readback.GetConstMappedRange(0, ROW * SIZE));
    if (!pixels) {
        say("FAIL render: no mapped memory");
        readback.Unmap();
        return false;
    }
    // inside the triangle (its middle, toward the base) red; a corner the clear colour's black
    const uint8_t *in = pixels + 40 * ROW + 32 * 4, *out = pixels + 2 * ROW + 2 * 4;
    const bool ok = in[0] == 255 && in[1] == 0 && in[2] == 0 && in[3] == 255 && out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 255;
    say("render: %s (inside %u,%u,%u,%u, corner %u,%u,%u,%u; %.0f ms in all)", ok ? "PASS" : "FAIL", in[0], in[1], in[2], in[3],
        out[0], out[1], out[2], out[3], ms_since(t));
    readback.Unmap();
    return ok;
}

void *selftest(void *)
{
    sleep(5);   // (WebKit's GL display, Zink's own Vulkan device, is up by now: the test runs beside it)
    g_start = Clock::now();
    mkdir(WISP_DATA "/run", 0777);
    if (FILE *f = fopen(RESULT_FILE, "w"))
        fclose(f);
    say("Dawn on the kit's RADV, beside Zink, in the GPU web process");

    static const auto TIMED_WAIT_ANY = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor instance_descriptor = {};
    instance_descriptor.requiredFeatureCount = 1;
    instance_descriptor.requiredFeatures = &TIMED_WAIT_ANY;
    wgpu::Instance instance = wgpu::CreateInstance(&instance_descriptor);
    if (!instance) {
        say("FAIL: no instance");
        return nullptr;
    }

    wgpu::RequestAdapterOptions adapter_options = {};
    adapter_options.backendType = wgpu::BackendType::Vulkan;
    adapter_options.powerPreference = wgpu::PowerPreference::HighPerformance;
    wgpu::Adapter adapter;
    std::string adapter_message;
    Clock::time_point t = Clock::now();
    wgpu::Future future = instance.RequestAdapter(&adapter_options, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter found, wgpu::StringView message) {
            if (status == wgpu::RequestAdapterStatus::Success)
                adapter = std::move(found);
            adapter_message = text(message);
        });
    instance.WaitAny(future, 30'000'000'000ull);
    if (!adapter) {
        say("FAIL: no Vulkan adapter (%s)", adapter_message.c_str());
        return nullptr;
    }
    wgpu::AdapterInfo info;
    adapter.GetInfo(&info);
    say("adapter in %.0f ms: %s (%s), %s, driver %s, vendor %04x device %04x", ms_since(t), text(info.device).c_str(),
        text(info.architecture).c_str(), backend_name(info.backendType), text(info.description).c_str(), info.vendorID, info.deviceID);
    wgpu::Limits limits;
    adapter.GetLimits(&limits);
    wgpu::SupportedFeatures features;
    adapter.GetFeatures(&features);
    say("limits: 2D textures %u, buffers %llu MiB, storage bindings %llu MiB, %u bind groups, %u workgroup invocations; %zu features",
        limits.maxTextureDimension2D, static_cast<unsigned long long>(limits.maxBufferSize >> 20),
        static_cast<unsigned long long>(limits.maxStorageBufferBindingSize >> 20), limits.maxBindGroups,
        limits.maxComputeInvocationsPerWorkgroup, features.featureCount);

    wgpu::DeviceDescriptor device_descriptor = {};
    device_descriptor.SetUncapturedErrorCallback([](const wgpu::Device &, wgpu::ErrorType, wgpu::StringView message) {
        say("device error: %s", text(message).c_str());
    });
    device_descriptor.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device &, wgpu::DeviceLostReason reason, wgpu::StringView message) {
            if (reason != wgpu::DeviceLostReason::Destroyed)
                say("device lost (%d): %s", static_cast<int>(reason), text(message).c_str());
        });
    wgpu::Device device;
    std::string device_message;
    t = Clock::now();
    future = adapter.RequestDevice(&device_descriptor, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::RequestDeviceStatus status, wgpu::Device made, wgpu::StringView message) {
            if (status == wgpu::RequestDeviceStatus::Success)
                device = std::move(made);
            device_message = text(message);
        });
    instance.WaitAny(future, 30'000'000'000ull);
    if (!device) {
        say("FAIL: no device (%s)", device_message.c_str());
        return nullptr;
    }
    say("device in %.0f ms (a second Vulkan device in this process, beside Zink's)", ms_since(t));

    const bool compute = compute_test(instance, device);
    const bool render = render_test(instance, device);
    device.Destroy();
    say("RESULT: %s (compute %s, render %s) in %.0f ms", compute && render ? "PASS" : "FAIL", compute ? "pass" : "fail",
        render ? "pass" : "fail", ms_since(g_start));
    return nullptr;
}

} // namespace

// The test on a thread of its own (8 MiB of stack: Tint's compiler recurses)
void wisp_webgpu_selftest_start()
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(8) << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, selftest, nullptr) == 0)
        pthread_detach(thread);
    else
        wisp_log("webgpu test: its thread did not start");
    pthread_attr_destroy(&attr);
}
