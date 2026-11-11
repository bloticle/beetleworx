#include "render_logging.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <spdlog/sinks/base_sink.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_BOOL(em2_coalesce_texture_warnings,true,"GPU","Coalesce identical invalid-texture warning messages in a bounded 128-entry cache; preserves validation");

namespace {
std::atomic<uint64_t> repeats{0};
class TextureWarningSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit TextureWarningSink(std::vector<spdlog::sink_ptr> downstream)
        : downstream_(std::move(downstream)) {}
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        const std::string_view text(message.payload.data(),message.payload.size());
        bool enabled=false;
        for(const auto& sink:downstream_) enabled|=sink->should_log(message.level);
        if(!enabled) return;
        if(REXCVAR_GET(em2_coalesce_texture_warnings) && message.level==spdlog::level::warn &&
           text.starts_with("Texture fetch constant (") &&
           text.find("has \"invalid\" type!")!=std::string_view::npos) {
            for(unsigned i=0;i<size_;++i) if(recent_[i]==text) {
                repeats.fetch_add(1,std::memory_order_relaxed);
                return;
            }
            recent_[next_]=text;
            next_=(next_+1)%recent_.size();
            if(size_<recent_.size()) ++size_;
        }
        for(const auto& sink:downstream_)
            if(sink->should_log(message.level)) sink->log(message);
    }
    void flush_() override { for(const auto& sink:downstream_) sink->flush(); }
    void set_pattern_(const std::string& pattern) override {
        for(const auto& sink:downstream_) sink->set_pattern(pattern);
    }
    void set_formatter_(std::unique_ptr<spdlog::formatter> formatter) override {
        for(const auto& sink:downstream_) sink->set_formatter(formatter->clone());
    }
    std::vector<spdlog::sink_ptr> downstream_;
    std::array<std::string,128> recent_;
    unsigned size_=0,next_=0;
};

class CaptureSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> messages;
    unsigned flushes=0;
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        messages.emplace_back(message.payload.data(),message.payload.size());
    }
    void flush_() override {++flushes;}
};
}

void em2::InstallRenderLogFilter() {
    // Install before starting guest/GPU threads. Preserve the existing sinks,
    // their levels/formatters, and every message outside this exact warning.
    if(auto logger=rex::GetLogger(rex::LogCategory::GPU)) {
        auto filter=std::make_shared<TextureWarningSink>(logger->sinks());
        logger->sinks()={std::move(filter)};
    }
}
void em2::LogRenderLogFilterStats() {
    const auto count=repeats.exchange(0,std::memory_order_relaxed);
    if(count) REXLOG_INFO("Coalesced {} repeated invalid texture-fetch warnings; new descriptors remain logged (128-entry cache)",count);
}
bool em2::TestRenderLogFilter() {
    const auto saved_setting=REXCVAR_GET(em2_coalesce_texture_warnings);
    const auto saved_repeats=repeats.exchange(0);
    REXCVAR_SET(em2_coalesce_texture_warnings,true);
    auto capture=std::make_shared<CaptureSink>();
    auto filter=std::make_shared<TextureWarningSink>(std::vector<spdlog::sink_ptr>{capture});
    auto send=[&](std::string_view text,spdlog::level::level_enum level=spdlog::level::warn) {
        filter->log(spdlog::details::log_msg("test",level,spdlog::string_view_t(text.data(),text.size())));
    };
    const std::string warning="Texture fetch constant (85004800 1B581006 002CE27F 00000C14 00000000 00000200) has \"invalid\" type!";
    send(warning); send(warning);
    bool ok=capture->messages==std::vector<std::string>{warning} && repeats.load()==1;
    send(warning,spdlog::level::err); send(warning,spdlog::level::err);
    send("Texture fetch constant (1) is completely invalid!");
    send("Texture fetch constant (1) is completely invalid!");
    ok&=capture->messages.size()==5;
    REXCVAR_SET(em2_coalesce_texture_warnings,false);
    send(warning); send(warning);
    ok&=capture->messages.size()==7;
    REXCVAR_SET(em2_coalesce_texture_warnings,true);
    for(unsigned i=0;i<128;++i) send(fmt::format("Texture fetch constant ({}) has \"invalid\" type!",i));
    send(warning); send(warning);
    ok&=capture->messages.size()==136 && capture->messages.back()==warning && repeats.load()==2;
    filter->flush(); ok&=capture->flushes==1;
    capture->set_level(spdlog::level::err);
    const std::string filtered="Texture fetch constant (new) has \"invalid\" type!";
    send(filtered);
    capture->set_level(spdlog::level::trace);
    send(filtered);
    ok&=capture->messages.size()==137 && capture->messages.back()==filtered;
    REXCVAR_SET(em2_coalesce_texture_warnings,saved_setting);
    repeats.store(saved_repeats);
    if(ok) REXLOG_INFO("Render log filter tests passed: repeats, distinct descriptors, eviction, levels, bypass and flush preservation");
    else REXLOG_ERROR("Render log filter tests failed");
    return ok;
}
