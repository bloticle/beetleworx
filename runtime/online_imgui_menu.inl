// Included inside engine_workbench.cpp's anonymous namespace. The desktop
// window shares the existing coordinator, transport, save handoff and checks.
thread_local int online_ui_fixture=-1;
thread_local bool online_ui_host_clicked=false;
ImVec2 online_host_min{},online_host_max{};
struct DesktopOnlineStart {int choice=0;std::string code;};
std::mutex desktop_start_mutex;
std::optional<DesktopOnlineStart> desktop_start;
void QueueDesktopOnlineStart(int choice,std::string code={}){
    std::lock_guard lock(desktop_start_mutex);
    if(!desktop_start)desktop_start=DesktopOnlineStart{choice,std::move(code)};
}
std::optional<DesktopOnlineStart> TakeDesktopOnlineStart(){
    std::lock_guard lock(desktop_start_mutex);auto result=std::move(desktop_start);desktop_start.reset();return result;
}
// Only called by the native GFx heartbeat on the original guest menu thread.
void DispatchDesktopOnlineStart(){
    if(auto request=TakeDesktopOnlineStart()){
        if(request->choice==20)em2::net::JoinNativeOnlineRoom(request->code);
        else {int result=0;em2::net::NativeOnlineMenuOption(2700,request->choice,result);}
    }
}

int OnlineOption(int id,int choice=-1){
    if(online_ui_fixture>=0){
        if(id==2700){if(choice==1)online_ui_host_clicked=true;return online_ui_fixture;}
        if(id==2743 || id==2731 || id==2720 || id==2702 || id==2706)return 1;
        if(id==2701)return online_ui_fixture;
        if(id==2770 || id==2736)return 16;
        if(id==2774)return choice==0?7:12;
        if(id==2772)return choice%2;
        if(id==2773)return int(em2::net::kLobbyColors[std::clamp(choice,0,15)].rgba>>8);
        std::string text;
        if(id==2710)text="Lobby status / desktop layout fixture";
        else if(id==2711)text="ABCD2345";
        else if(id==2771){text="Player "+std::to_string(choice/128+1);choice%=128;}
        return choice>=0 && size_t(choice)<text.size()?uint8_t(text[size_t(choice)]):0;
    }
    if(id==2700 && (choice==1 || choice==2 || choice==4 || choice==12)){
        QueueDesktopOnlineStart(choice);return 0;
    }
    int value=0;em2::net::NativeOnlineMenuOption(id,choice,value);return value;
}
int OnlineChatOption(int id,int choice=-1){int value=0;em2::net::NativeOnlineChatOption(id,choice,value);return value;}
int OnlineVoiceOption(int id,int choice=-1){int value=0;em2::net::NativeOnlineVoiceOption(id,choice,value);return value;}
std::string OnlineText(int id,int start=0){
    std::string text;for(int i=0;i<400;++i){const int c=OnlineOption(id,start+i);if(c<=0)break;text+=char(c);}return text;
}
std::string OnlineChatText(int id){
    std::string text;for(int i=0;i<512;++i){const int c=OnlineChatOption(id,i);if(c<=0)break;text+=char(c);}return text;
}
ImVec4 OnlineColor(unsigned rgb){return ImVec4(float((rgb>>16)&255)/255,float((rgb>>8)&255)/255,float(rgb&255)/255,1);}
char online_code[64]{},online_name[100]{},online_message[241]{};
bool online_profile_loaded=false,online_open_kick=false;
std::string online_ui_error;
void CloseWorkbench(){
    TakeDesktopOnlineStart();
    if(requested.load()==Panel::Online)OnlineOption(2700,6);
    SetPanel(Panel::Closed);
}
void OpenOnlineWorkbench(){
    if(OnlineOption(2743)!=1)return;
    em2::net::CloseOnlineChat();em2::net::CancelNativeOnlineNameInput();
    online_profile_loaded=false;online_open_kick=false;online_ui_error.clear();online_message[0]=0;
    OnlineOption(2700,0);SetPanel(Panel::Online);
}
void DrawOnlineVoice(){
    if(!ImGui::CollapsingHeader("Voice chat"))return;
    int mode=OnlineVoiceOption(2800);
    if(ImGui::Combo("Microphone mode",&mode,"Off\0Listen only\0Push to talk\0Open microphone\0"))OnlineVoiceOption(2800,mode);
    int positioning=OnlineVoiceOption(2804);
    if(ImGui::Combo("Voice positioning",&positioning,"Party\0Proximity\0"))OnlineVoiceOption(2804,positioning);
    int range=OnlineVoiceOption(2805);
    if(positioning && ImGui::Combo("Proximity range",&range,"10\0 20\0 30\0 50\0 100\0"))OnlineVoiceOption(2805,range);
    int volume=OnlineVoiceOption(2801);
    if(ImGui::SliderInt("Playback volume",&volume,0,8,"%d / 4"))OnlineVoiceOption(2801,volume);
    int key=OnlineVoiceOption(2802);
    if(ImGui::Combo("Push-to-talk key",&key,"F8\0Caps Lock\0Alt\0"))OnlineVoiceOption(2802,key);
    auto voiceText=[](int id){std::string text;for(int i=0;i<400;++i){int c=OnlineVoiceOption(id,i);if(c<=0)break;text+=char(c);}return text;};
    for(const auto& entry:std::array<std::pair<int,const char*>,2>{{{2806,"Microphone"},{2807,"Speakers"}}}){
        const auto label=std::string(entry.second)+": "+voiceText(entry.first+2);
        if(ImGui::Button(label.c_str()))OnlineVoiceOption(entry.first,1);
    }
    if(ImGui::Button("Refresh audio devices"))OnlineVoiceOption(2810,1);
    const auto status=voiceText(2803);ImGui::TextWrapped("%s",status.c_str());
}
void DrawOnlineLobby(){
    const int state=OnlineOption(2701);
    if(state==7){SetPanel(Panel::Closed);return;} // Original native save/Continue handoff owns gameplay.
    if(OnlineOption(2743)!=1){CloseWorkbench();return;}
    ImGui::TextUnformatted("ONLINE MULTIPLAYER");
    ImGui::SameLine();ImGui::TextDisabled("Invite-only rooms / up to 16 players");
    ImGui::Separator();
    const auto status=OnlineText(2710);
    if(state==9)ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.55f,.45f,1));
    ImGui::TextWrapped("%s",status.c_str());
    if(state==9)ImGui::PopStyleColor();
    if(!online_ui_error.empty())ImGui::TextWrapped("%s",online_ui_error.c_str());
    const bool idle=state==0 || state==9;
    const bool lobby=(state==2 || state==6) && OnlineOption(2731)==1;
    if(idle){
        ImGui::Spacing();
        if(ImGui::BeginTable("connection-actions",2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_BordersInnerV)){
            ImGui::TableNextColumn();ImGui::SeparatorText("Host a game");
            ImGui::TextWrapped("Use your current save. Your friends join with a short room code or a Discord invitation.");
            if(ImGui::Button("Host Game",ImVec2(-1,44))){online_profile_loaded=false;online_ui_error.clear();OnlineOption(2700,1);}
            if(online_ui_fixture>=0){online_host_min=ImGui::GetItemRectMin();online_host_max=ImGui::GetItemRectMax();}
            ImGui::TableNextColumn();ImGui::SeparatorText("Join a friend");
            ImGui::TextWrapped("Ask the host for their eight-character room code. No router configuration is required.");
            ImGui::SetNextItemWidth(-1);
            const bool enter=ImGui::InputTextWithHint("##room-code","Room code",online_code,sizeof(online_code),ImGuiInputTextFlags_CharsUppercase|ImGuiInputTextFlags_EnterReturnsTrue);
            if(ImGui::Button("Paste code")){if(const auto* text=ImGui::GetClipboardText())snprintf(online_code,sizeof(online_code),"%s",text);}
            ImGui::SameLine();
            if(ImGui::Button("Join Game") || enter){online_profile_loaded=false;online_ui_error.clear();QueueDesktopOnlineStart(20,online_code);}
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if(ImGui::CollapsingHeader("Resume an online save")){
            if(ImGui::Button("Find saved online games"))OnlineOption(2700,11);
            const auto saved=OnlineText(2713);ImGui::TextWrapped("%s",saved.c_str());
            const int count=OnlineOption(2708);
            if(count){
                if(ImGui::Button("Previous save"))OnlineOption(2700,13);ImGui::SameLine();
                if(ImGui::Button("Next save"))OnlineOption(2700,14);ImGui::SameLine();
                ImGui::BeginDisabled(OnlineOption(2714)!=0);
                if(ImGui::Button("Host selected save")){online_profile_loaded=false;OnlineOption(2700,12);}
                ImGui::EndDisabled();
            }
        }
    }else if(lobby){
        const bool host=OnlineOption(2702)!=0;
        const auto code=OnlineText(2711);
        ImGui::SeparatorText("Invite friends");
        ImGui::Text("Room code: %s",code.empty()?"Preparing...":code.c_str());ImGui::SameLine();
        ImGui::BeginDisabled(code.empty());if(ImGui::Button("Copy code"))ImGui::SetClipboardText(code.c_str());ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(OnlineOption(2745,0)!=1);
        if(ImGui::Button("Invite Friends on Discord"))OnlineOption(2745,1);
        ImGui::EndDisabled();
        const int count=std::clamp(OnlineOption(2770),0,16);
        int local_character=0;unsigned local_rgb=0;
        for(int i=0;i<count;++i)if(OnlineOption(2774,i)&2){local_character=OnlineOption(2772,i);local_rgb=unsigned(OnlineOption(2773,i));
            if(!online_profile_loaded){const auto name=OnlineText(2771,i*128);snprintf(online_name,sizeof(online_name),"%s",name.c_str());online_profile_loaded=true;}}
        if(ImGui::BeginTable("lobby-columns",2,ImGuiTableFlags_Resizable|ImGuiTableFlags_SizingStretchProp)){
            ImGui::TableSetupColumn("Players",ImGuiTableColumnFlags_WidthStretch,1.15f);
            ImGui::TableSetupColumn("Chat",ImGuiTableColumnFlags_WidthStretch,1.f);
            ImGui::TableNextColumn();ImGui::Text("Players (%d/%d)",count,OnlineOption(2736));
            const float height=std::clamp(ImGui::GetContentRegionAvail().y-250.f,150.f,350.f);
            if(ImGui::BeginTable("players",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY,ImVec2(0,height))){
                ImGui::TableSetupColumn("Name",ImGuiTableColumnFlags_WidthStretch,1.6f);ImGui::TableSetupColumn("Character");ImGui::TableSetupColumn("Status");ImGui::TableSetupColumn("##actions",ImGuiTableColumnFlags_WidthFixed,42);
                ImGui::TableHeadersRow();
                for(int i=0;i<count;++i){
                    ImGui::PushID(i);ImGui::TableNextRow();const int flags=OnlineOption(2774,i);const auto name=OnlineText(2771,i*128);
                    ImGui::TableNextColumn();ImGui::TextColored(OnlineColor(unsigned(OnlineOption(2773,i))),"%s%s",name.c_str(),flags&2?" (you)":"");
                    ImGui::TableNextColumn();ImGui::TextUnformatted(OnlineOption(2772,i)==0?"Mickey":"Oswald");
                    ImGui::TableNextColumn();ImGui::TextUnformatted(flags&1?"Host":flags&4?"Ready":"Not ready");
                    ImGui::TableNextColumn();if((flags&8) && ImGui::SmallButton("Kick")){if(OnlineOption(2775,i)==1)online_open_kick=true;}
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TableNextColumn();ImGui::TextUnformatted("Lobby chat");
            OnlineChatOption(2751);const int messages=OnlineChatOption(2752);
            ImGui::BeginChild("chat-history",ImVec2(0,height),ImGuiChildFlags_Borders);
            const bool bottom=ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-8;
            for(int i=0;i<messages;++i){OnlineChatOption(2753,i);const auto name=OnlineChatText(2754),text=OnlineChatText(2755);
                ImGui::TextColored(OnlineColor(unsigned(OnlineChatOption(2756))),"%s",name.c_str());ImGui::SameLine();ImGui::TextWrapped("%s",text.c_str());}
            if(bottom)ImGui::SetScrollHereY(1);ImGui::EndChild();
            ImGui::SetNextItemWidth(-70);
            const bool send=ImGui::InputTextWithHint("##chat-message","Type a message",online_message,sizeof(online_message),ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();if(ImGui::Button("Send") || send){if(em2::net::SubmitOnlineLobbyChat(online_message)){online_message[0]=0;online_ui_error.clear();}else online_ui_error="Message could not be sent. Check the connection or try a shorter message.";}
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Your player");
        const bool editable=OnlineOption(2720)!=0;
        ImGui::BeginDisabled(!editable);
        ImGui::SetNextItemWidth(220);const bool rename=ImGui::InputText("Name",online_name,sizeof(online_name),ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();if(ImGui::Button("Save name") || rename){if(!em2::net::RenameNativeOnlinePlayer(online_name))online_ui_error="Name could not be changed. Use 1-24 characters.";else online_ui_error.clear();}
        ImGui::BeginDisabled(OnlineOption(2737)!=1);int character=local_character;
        ImGui::SetNextItemWidth(160);if(ImGui::Combo("Character",&character,"Mickey\0Oswald\0")){
            int color=0;for(int i=0;i<int(em2::net::kLobbyColors.size());++i)if(em2::net::kLobbyColors[i].rgba>>8==local_rgb)color=i;
            em2::net::SetNativeOnlineAppearance(character,color);
        }ImGui::EndDisabled();
        ImGui::TextUnformatted("Pants and name color (one player per color)");
        for(int i=0;i<int(em2::net::kLobbyColors.size());++i){
            const auto& color=em2::net::kLobbyColors[i];const auto rgb=color.rgba>>8;
            bool used=false;for(int j=0;j<count;++j)if(!(OnlineOption(2774,j)&2) && unsigned(OnlineOption(2773,j))==rgb)used=true;
            ImGui::PushID(100+i);ImGui::BeginDisabled(used);
            if(ImGui::ColorButton("##color",OnlineColor(rgb),ImGuiColorEditFlags_NoTooltip,ImVec2(26,26)))em2::net::SetNativeOnlineAppearance(local_character,i);
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%.*s%s",int(color.name.size()),color.name.data(),used?" (in use)":rgb==local_rgb?" (selected)":"");
            ImGui::EndDisabled();ImGui::PopID();if((i+1)%8)ImGui::SameLine();
        }ImGui::EndDisabled();
        const auto profile_status=OnlineText(2726);if(!profile_status.empty())ImGui::TextWrapped("%s",profile_status.c_str());
        ImGui::Separator();
        ImGui::BeginDisabled(host?OnlineOption(2706)!=1:!editable);
        if(ImGui::Button(host?"Start Game":OnlineOption(2721)?"Not ready":"Ready",ImVec2(180,36))){if(host)OnlineOption(2700,7);else OnlineOption(2725,0);}
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Leave lobby",ImVec2(140,36))){OnlineOption(2700,6);online_profile_loaded=false;}
    }else{
        ImGui::Spacing();ImGui::TextUnformatted("Please wait. File preparation and connection run in the background.");
        if(ImGui::Button("Cancel connection")){OnlineOption(2700,6);online_profile_loaded=false;}
    }
    if(online_open_kick){ImGui::OpenPopup("Remove player?");online_open_kick=false;}
    if(ImGui::BeginPopupModal("Remove player?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        const auto name=OnlineText(2776);
        ImGui::Text("Remove %s?",name.empty()?"this player":name.c_str());
        if(ImGui::Button("Kick player")){OnlineOption(2777,1);ImGui::CloseCurrentPopup();}ImGui::SameLine();
        if(ImGui::Button("Cancel")){OnlineOption(2777,0);ImGui::CloseCurrentPopup();}ImGui::EndPopup();
    }
    ImGui::Spacing();DrawOnlineVoice();
    ImGui::TextDisabled("Esc / controller Start: close   |   Internet rooms require matching demo versions.");
}
