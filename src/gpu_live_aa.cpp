#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

extern "C" __declspec(dllexport) void stfr_set_gpu_post_effect(void* graphics, int mode)
{
    if (!graphics) return;
    auto* system = static_cast<rex::graphics::GraphicsSystem*>(
        static_cast<rex::system::IGraphicsSystem*>(graphics));
    auto* processor = system->command_processor();
    if (!processor) return;

    using Effect = rex::graphics::CommandProcessor::SwapPostEffect;
    const Effect effect = mode == 1 ? Effect::kFxaa :
                          mode == 2 ? Effect::kFxaaExtreme : Effect::kNone;
    processor->SetDesiredSwapPostEffect(effect);
}
