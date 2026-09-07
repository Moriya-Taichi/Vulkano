#include "engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <cstdlib>
using namespace vulkano;
namespace {
int checks = 0;
void expect(bool condition, const char* label) { ++checks; if (!condition) throw std::runtime_error(label); }
template<class F> void rejects(F f, const char* label) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    expect(rejected, label);
}
Shader shader(const char* name) {
    std::ifstream in(std::string(VULKANO_TEST_ASSETS) + "/" + name, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error(std::string("Missing shader ") + name);
    auto length = in.tellg(); Shader result; result.code.resize(static_cast<size_t>(length) / 4);
    in.seekg(0); in.read(reinterpret_cast<char*>(result.code.data()), length); return result;
}
std::vector<uint8_t> integer(uint32_t n) { std::vector<uint8_t> bytes(4); std::memcpy(bytes.data(), &n, 4); return bytes; }
std::shared_ptr<Buffer> buffer(const std::shared_ptr<Device>& d, uint64_t size, Storage storage = Storage::Shared) {
    return std::make_shared<Buffer>(d, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, storage);
}
std::shared_ptr<Texture> texture(const std::shared_ptr<Device>& d, VkImageUsageFlags usage) {
    return std::make_shared<Texture>(d, 16, 16, VK_FORMAT_R8G8B8A8_UNORM, usage, Storage::Private);
}
}
int main() try {
    auto d = Device::create(0, std::getenv("VULKANO_VALIDATION") != nullptr, true);
    std::cout << "Device: " << d->properties.deviceName << '\n';
    expect(d->enabled == 0, "Optional features must be opt-in");
    rejects([&] { Device::create(1u << 30, false, true); }, "Unknown feature must fail");
    auto other = Device::create(0, false, true);
    auto foreign = buffer(other, 16);
    auto source = buffer(d, 1028), gpu = buffer(d, 1028, Storage::Private), output = buffer(d, 1028);
    std::vector<float> values(257); for (size_t i = 0; i < values.size(); ++i) values[i] = float(i) + 0.5f;
    source->write(0, values.data(), 1028);
    rejects([&] { source->write(UINT64_MAX, values.data(), 4); }, "Overflowing write must fail");
    rejects([&] { gpu->read(0, values.data(), 4); }, "Private buffers must not map");
    auto c = std::make_shared<Command>(d);
    rejects([&] { c->copy(source, foreign, 0, 0, 4); }, "Cross-device copy must fail");
    rejects([&] { c->copy(source, output, 1, 0, 4); }, "Unaligned copy must fail");
    c->copy(source, gpu, 0, 0, 1028);
    auto compute = std::make_shared<Pipeline>(d, std::vector<BindingLayout>{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}, 4, shader("double.comp.spv"));
    expect(compute->localSize[0] == 64, "SPIR-V workgroup reflection");
    auto inferred = std::make_shared<Pipeline>(d, std::vector<BindingLayout>{}, 0, shader("double.comp.spv"));
    expect(inferred->pushBytes == 4 && inferred->bindings.size() == 1, "Automatic pipeline layout reflection");
    rejects([&] { std::make_shared<Pipeline>(d, std::vector<BindingLayout>{{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}}, 4, shader("double.comp.spv")); }, "Mismatched shader layout must fail");
    rejects([&] { std::make_shared<Pipeline>(d, std::vector<BindingLayout>{}, 0, shader("half.comp.spv")); }, "Disabled Float16 shader must fail");
    if (d->available & Float16) {
        auto halfDevice = Device::create(Float16, std::getenv("VULKANO_VALIDATION") != nullptr, true);
        auto halfPipeline = std::make_shared<Pipeline>(halfDevice, std::vector<BindingLayout>{}, 0, shader("half.comp.spv"));
        auto halfBuffer = buffer(halfDevice, 16);
        float numbers[] = {1, 2, 3, 4}; halfBuffer->write(0, numbers, sizeof(numbers));
        Binding hb{}; hb.buffer = halfBuffer; hb.index = 0; hb.length = 16;
        auto hc = std::make_shared<Command>(halfDevice); hc->dispatch({halfPipeline, {hb}, {}, {1, 1, 1}}); hc->commit(); hc->wait();
        halfBuffer->read(0, numbers, sizeof(numbers));
        expect(numbers[0] == 2 && numbers[3] == 8, "Enabled Float16 shader must execute");
    }
    Binding b{}; b.index = 0; b.buffer = gpu; b.length = 1028;
    Dispatch op{compute, {b}, integer(257), {5, 1, 1}};
    auto invalid = op; invalid.groups[0] = d->properties.limits.maxComputeWorkGroupCount[0] + 1;
    rejects([&] { c->dispatch(invalid); }, "Oversized dispatch must fail");
    invalid = op; invalid.bindings[0].index = 3;
    rejects([&] { c->dispatch(invalid); }, "Wrong binding index must fail");
    invalid = op; invalid.constants.clear();
    rejects([&] { c->dispatch(invalid); }, "Missing constants must fail");
    c->dispatch(op); c->dispatch(op); c->copy(gpu, output, 0, 0, 1028);
    source.reset(); // Recorded native resources must survive release before submission.
    c->commit();
    rejects([&] { c->commit(); }, "Double submission must fail");
    c->wait(); output->read(0, values.data(), 1028);
    for (size_t i = 0; i < values.size(); ++i) expect(values[i] == (float(i) + 0.5f) * 4, "Compute/barrier/readback mismatch");
    std::cout << "Compute, buffer transfer and lifetime checks passed\n";

    // Record the consumer before the producer, then submit in dependency order.
    // Layouts must be resolved at commit, not during recording.
    auto image = texture(d, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    auto target = texture(d, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    auto upload = buffer(d, 1024), readback = buffer(d, 1024);
    std::vector<uint8_t> pixels(1024);
    for (size_t i = 0; i < pixels.size(); i += 4) { pixels[i] = 17; pixels[i + 1] = 51; pixels[i + 2] = 85; pixels[i + 3] = 255; }
    upload->write(0, pixels.data(), pixels.size());
    auto invert = std::make_shared<Pipeline>(d, std::vector<BindingLayout>{{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}, 0, shader("invert.comp.spv"));
    Binding imageBinding{}; imageBinding.index = 0; imageBinding.texture = image;
    auto consumer = std::make_shared<Command>(d); consumer->dispatch({invert, {imageBinding}, {}, {2, 2, 1}});
    auto producer = std::make_shared<Command>(d); producer->copy(upload, image, 0, true); producer->commit(); consumer->commit();
    auto sampler = std::make_shared<Sampler>(d, false, false, 1);
    auto renderPipeline = std::make_shared<Pipeline>(d, std::vector<BindingLayout>{{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}}, 0,
        shader("fullscreen.vert.spv"), shader("sample.frag.spv"), VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, false);
    auto depth = std::make_shared<Texture>(d, 16, 16, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, Storage::Memoryless);
    Render pass{}; pass.color = target; pass.depth = depth; imageBinding.sampler = sampler;
    pass.draws.push_back({renderPipeline, {imageBinding}, {}, 3, 1, 0, 0});
    auto graphics = std::make_shared<Command>(d); graphics->render(pass); graphics->copy(readback, target, 0, false); graphics->commit(); graphics->wait();
    readback->read(0, pixels.data(), pixels.size());
    for (size_t i = 0; i < pixels.size(); i += 4) {
        expect(pixels[i] == 238 && pixels[i + 1] == 204 && pixels[i + 2] == 170 && pixels[i + 3] == 255, "Texture compute/sample/render/readback mismatch");
    }
    expect(!depth->initialized, "Memoryless depth content must be discarded");
    auto badPass = pass; badPass.depthStore = VK_ATTACHMENT_STORE_OP_STORE;
    auto bad = std::make_shared<Command>(d);
    rejects([&] { bad->render(badPass); }, "Memoryless store must fail");
    rejects([&] { std::make_shared<Texture>(d, 4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, Storage::Memoryless); }, "Memoryless sampling must fail");
    std::cout << "Texture transfer, compute, sampling, graphics and transient depth passed\n";

    auto fresh = texture(d, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    auto failed = std::make_shared<Command>(d);
    Render clear{}; clear.color = fresh; clear.clearColor = {0, 1, 0, 1}; failed->render(clear);
    // Following read is invalid. Failed recording must not publish earlier layouts.
    auto uninitialized = texture(d, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    failed->copy(readback, uninitialized, 0, false);
    rejects([&] { failed->commit(); }, "Uninitialized texture read must fail");
    expect(failed->state == Command::State::Failed && fresh->layout == VK_IMAGE_LAYOUT_UNDEFINED && !fresh->initialized, "Failed commit must roll back image state");
    auto clearCommand = std::make_shared<Command>(d); clearCommand->render(clear); clearCommand->commit();
    Render load = clear; load.colorLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
    auto loadCommand = std::make_shared<Command>(d); loadCommand->render(load); loadCommand->copy(readback, fresh, 0, false); loadCommand->commit(); loadCommand->wait();
    readback->read(0, pixels.data(), pixels.size());
    expect(pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0 && pixels[3] == 255, "Attachment LOAD must preserve content");
    auto discard = std::make_shared<Command>(d); clear.colorStore = VK_ATTACHMENT_STORE_OP_DONT_CARE; discard->render(clear); discard->commit(); discard->wait();
    auto readDiscarded = std::make_shared<Command>(d); readDiscarded->copy(readback, fresh, 0, false);
    rejects([&] { readDiscarded->commit(); }, "Discarded attachment must not be read");
    d->waitIdle();
    std::cout << "PASS: " << checks << " checks\n";
    return 0;
} catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
