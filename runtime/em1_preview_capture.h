#pragma once
namespace rex::ui { class Presenter; }
namespace em2::em1 {
// Game-thread readiness notification; does not read back graphics or capture OS UI.
void NotifyArchitecturePreviewCaptureReady() noexcept;
bool ArchitecturePreviewGraphicsReady() noexcept;
// Read-only draw diagnostics also cover complete registered EM1 levels.
// This flag does not enable preview rendering or framebuffer capture.
void NotifyRegisteredEm1GraphicsReady(bool) noexcept;
bool Em1DrawDiagnosticsReady() noexcept;
bool ArchitecturePreviewCapturePending() noexcept;
// Call from the application's UI tick with its current presenter. Opt-in,
// one-shot, waits two seconds after readiness, refuses existing output files.
// Invoke outside the presenter paint stack (for example a deferred UI callback).
void TickArchitecturePreviewCapture(rex::ui::Presenter& presenter) noexcept;
}
