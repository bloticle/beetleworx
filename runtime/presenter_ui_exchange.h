#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
namespace rex::ui {class Presenter;class Window;class ImmediateDrawer;class ImGuiDrawer;}
namespace rex::graphics {class GraphicsSystem;}
namespace em2 {
class Em2CommandProcessor;
// Construct on the host UI thread. A worker may block in Exchange only while
// native GPU producers are paused. Close before waiting for title/GPU shutdown.
class PresenterUiExchange {
public:
 using Post=std::function<void(std::function<void()>)>;
 using Handler=std::function<bool(std::unique_ptr<rex::ui::Presenter>&)>;
 PresenterUiExchange(Post,Handler,std::chrono::milliseconds queued_timeout=std::chrono::seconds(2));
 ~PresenterUiExchange();
 PresenterUiExchange(const PresenterUiExchange&)=delete;
 PresenterUiExchange& operator=(const PresenterUiExchange&)=delete;
 bool Exchange(std::unique_ptr<rex::ui::Presenter>&);
 void Close();
private:struct State;std::shared_ptr<State> state_;
};
// UI thread only, with producers paused. Failure restores the original window,
// drawers, paint settings and presenter ownership. An unrecoverable rollback
// throws so the coordinating operation retains resources and stops rendering.
bool ExchangeWindowPresenter(rex::graphics::GraphicsSystem&,rex::ui::Window&,rex::ui::ImmediateDrawer*,rex::ui::ImGuiDrawer*,std::unique_ptr<rex::ui::Presenter>&);
bool DesktopHdrEnabledFromUIThread(rex::ui::Presenter*);
bool TestPresenterUiExchange(Em2CommandProcessor&,const std::filesystem::path&);
}
