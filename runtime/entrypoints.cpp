#include "ppc_context.h"
#include "gpu_interrupt_lifetime.h"
#include "native_sampler.h"
#include "online_runtime.h"
#include <rex/kernel/xthread.h>
#include <rex/runtime/thread_state.h>
#include <stdexcept>

PPC_EXTERN_FUNC(__imp___xstart);
PPC_EXTERN_FUNC(__imp__sub_826D0C30);
PPC_EXTERN_FUNC(__imp__sub_826B54E0);

namespace {
void RunThreadEntry(PPCContext& initial, uint8_t* base, PPCFunc* implementation) {
    auto* thread = rex::kernel::XThread::GetCurrentThread();
    auto* state = thread ? thread->thread_state() : nullptr;
    if (!state) throw std::runtime_error("Guest entry has no runtime thread state");

    // SDK 0.1 starts guest code with a stack-local context, but dispatches APCs
    // with ThreadState::context(). Its stale stack pointer lets callbacks
    // overwrite active guest frames. Execute on the runtime-owned context so
    // callbacks and ordinary guest calls share the same current stack.
    auto* active = state->context();
    if (active != &initial) *active = initial;
    // Host GPU threads also dispatch guest interrupts. Their ThreadState
    // starts with a zero-initialized context in SDK 0.1, unlike guest startup.
    // Kernel imports must see the owning KernelState on both kinds of entry.
    active->kernel_state = thread->kernel_state();
    struct Binding {
        rex::runtime::ThreadState* previous = rex::runtime::ThreadState::Get();
        explicit Binding(rex::runtime::ThreadState* state) { rex::runtime::ThreadState::Bind(state); }
        ~Binding() { rex::runtime::ThreadState::Bind(previous); }
    } binding(state);
    implementation(*active, base);
    if (active != &initial) initial = *active;
}
}

PPC_FUNC_IMPL(_xstart) { em2::net::BindNativeOnlineThread();em2::NativeMainSampleScope sample;RunThreadEntry(ctx, base, __imp___xstart); }
PPC_FUNC_IMPL(sub_826D0C30) { RunThreadEntry(ctx, base, __imp__sub_826D0C30); }
PPC_FUNC_IMPL(sub_826B54E0) {
    // Drain this entry before title teardown, including its ThreadState binding.
    // The native callback queries process state via 826B8AF8; GPU workers can
    // remain alive after the process has been cleared by TerminateTitle.
    em2::DispatchGuestGPUInterrupt(ctx,base,[](PPCContext& current,uint8_t* memory){RunThreadEntry(current,memory,__imp__sub_826B54E0);});
}
