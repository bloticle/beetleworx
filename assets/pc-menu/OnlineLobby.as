   function _em2OnlineRead(id,offset)
   {
      if(offset == undefined) offset = 0;
      var text = ""; var escaped = ""; var hex = "0123456789ABCDEF"; var i = 0;
      while(i < 400)
      {
         var value = _level0.SetOption(id,offset+i);
         if(value == undefined || value <= 0) break;
         text += String.fromCharCode(value);
         escaped += "%" + hex.charAt(value >> 4) + hex.charAt(value & 15);
         i++;
      }
      try { return decodeURIComponent(escaped); } catch(error) { return text; }
   }
   function _em2OnlineButton(index,label,enabled)
   {
      var button = this._items[index];
      if(button._em2SavedLayout == undefined) button._em2SavedLayout={buttonX:button._x,y:button._y,x:button.t_label._x,width:button.t_label._width,autoSize:button.t_label.autoSize,format:button.t_label.getNewTextFormat(),hitX:button.hitarea._x,hitWidth:button.hitarea._width,hitY:button.hitarea._y,hitHeight:button.hitarea._height};
      {
         var width=Number(_global.initVars.screenWidth); if(!(width>=640)) width=1280;
         var height=Number(_global.initVars.screenHeight); if(!(height>=480)) height=720;
         var offset=Number(_global.offsetX); if(isNaN(offset)) offset=0;
         var rect=this._em2ActionRect(index);
         var saved=button._em2SavedLayout;
         var center=button.labelclip._x+(saved.x+saved.width/2)*button.labelclip._xscale/100;
         var target={x:offset+width*(rect.x+rect.width/2),y:height*rect.y};
         this.localToGlobal(target); button._parent.globalToLocal(target);
         button._x=target.x-center*button._xscale/100;
         button._y=target.y-(button.labelclip._y+button.t_label._y)*button._yscale/100;
         button._em2LabelWidth=width*rect.width/(button._xscale/100)/(button.labelclip._xscale/100);
         button._em2LabelX=saved.x+saved.width/2-button._em2LabelWidth/2;
      }
      button._visible = enabled; button._alpha = 100; button.label = label;
      if(enabled) button.enable(); else button.disable();
      var field=button.t_label; field.autoSize="none"; field._x=button._em2LabelX;
      field._width=button._em2LabelWidth; field.multiline=false; field.wordWrap=false;
      button.hitarea._x=button.labelclip._x+field._x*button.labelclip._xscale/100;
      button.hitarea._width=field._width*button.labelclip._xscale/100;
      button.hitarea._y=button.labelclip._y+field._y-4; button.hitarea._height=40;
      // Keep the authored EM2 button artwork and rollover timeline.
      field.embedFonts=false;
      var format=field.getNewTextFormat(); format.size=24; format.align="center"; field.setTextFormat(format);
      while(field.textWidth>field._width-8 && format.size>16) { format.size--; field.setTextFormat(format); }
      var full=label;
      while(field.textWidth>field._width-8 && full.length>1) { full=full.substr(0,full.length-1); field.text=full+"..."; }
      var name="_em2OnlineInfo"+index;
      if(this[name]==undefined) {
         var point={x:button._em2LabelX,y:field._y};
         button.labelclip.localToGlobal(point); this.globalToLocal(point);
         this.createTextField(name,this.getNextHighestDepth(),point.x,point.y,button._em2LabelWidth,40);
         this[name].embedFonts=false; this[name].selectable=false;
      }
      var info=this[name]; info._visible=!enabled; info.setNewTextFormat(format);
      var infoPoint={x:button._em2LabelX,y:field._y}; button.labelclip.localToGlobal(infoPoint); this.globalToLocal(infoPoint);
      info._x=infoPoint.x; info._y=infoPoint.y; info._width=button._em2LabelWidth;
      info.textColor=16777215; info.text=field.text;
   }
   function _em2OnlineOpen()
   {
      if(_level0.SetOption(2743,-1) != 1) { this.b_5.disable(); this.b_5._visible = false; return; }
      this._em2Online = true;
      this._em2JoinPage=false; this._em2KickConfirm=false;
      this._em2SavedNavigation=this._list.onKeyDown; this._list.onKeyDown=function() {};
      this._em2OnlineHostPage = false;
      this._em2OnlineVoicePage = 0;
      this._em2OnlinePlayerPage = false;
      this._em2OnlineRosterPage = false;
      _level0.SetOption(2700,0);
      if(this._em2OnlineHelp == undefined)
      {
         var width = _global.initVars.screenWidth;
         this.createTextField("_em2OnlineHelp",this.getNextHighestDepth(),_global.offsetX + width*0.1,this.b_select._y - 86,width*0.8,76);
         var format = this.b_main.t_label.getNewTextFormat(); format.size = 16; format.align = "center";
         this._em2OnlineHelp.embedFonts = false; this._em2OnlineHelp.setNewTextFormat(format);
         this._em2OnlineHelp.multiline = true; this._em2OnlineHelp.wordWrap = true;
         this._em2OnlineHelp.selectable = false;
      }
      this._em2OnlineHelp._visible = true;
      this._em2OnlineRefreshTime = 0;
      this._em2OnlineRefresh();
      this._list.activate(0);
      this._em2OnlineKeyListener = {onKeyDown:mx.utils.Delegate.create(this,this._em2OnlineKeyDown)};
      Key.addListener(this._em2OnlineKeyListener);
      this._em2OnlinePump = this.createEmptyMovieClip("_em2OnlinePump",this.getNextHighestDepth());
      this._em2OnlinePump.onEnterFrame = function() { this._parent._em2OnlineTick(); };
   }
   function _em2OnlineClose()
   {
      this._em2OnlineRestoreRosterColors();
      if(this._em2OnlineEditingName) this._em2OnlineEndName(false);
      _level0.SetOption(2700,6);
      this._em2Online = false;
      this._list.onKeyDown=this._em2SavedNavigation;
      if(this._em2OnlinePump != undefined) this._em2OnlinePump.removeMovieClip();
      Key.removeListener(this._em2OnlineKeyListener);
      if(this._em2OnlineHelp != undefined) this._em2OnlineHelp._visible = false;
      var statusIndex=0; while(statusIndex<6) { this["_em2OnlineInfo"+statusIndex]._visible=false; statusIndex++; }
      this._em2RestoreLayout();
      this._setText();
      var i = 0; while(i < this._items.length) { this._items[i].enable(); this._items[i]._visible = true; i++; }
      if(!_level0.IsSavedGamePresent()) { this.b_main.disable(); this.b_main._visible = false; }
      if(_level0.SetOption(2743,-1) != 1) { this.b_5.disable(); this.b_5._visible = false; this._list.activate(0); }
      else this._list.activate(5);
   }
   function _em2OnlineRefresh()
   {
      if(!this._em2Online || this._em2OnlineEditingName) return;
      if(_level0.SetOption(2743,-1) != 1) { this._em2OnlineClose(); return; }
      var state = _level0.SetOption(2701,-1);
      this._em2OnlineState = state;
      var idle = state == 0 || state == 9;
      var internet = _level0.SetOption(2743,1) == 1;
      if(this._em2JoinPage && _level0.SetOption(2780,-1)!=1) this._em2JoinPage=false;
      this._em2OnlineLayout(state);
      if(this._em2JoinPage) {
         this._em2OnlineHeading.text="Join Game";
         this._em2OnlineButton(0,"Join",true); this._em2OnlineButton(1,"Paste room code",true); this._em2OnlineButton(2,"Cancel",true);
         var joinHide=3; while(joinHide<6) { this._items[joinHide].disable(); this._items[joinHide]._visible=false; this["_em2OnlineInfo"+joinHide]._visible=false; joinHide++; }
         if(this._em2JoinCode==undefined) this._em2Field(this,"_em2JoinCode",this._em2OnlineHelp._x,this._em2OnlineHelp._y+52,this._em2OnlineHelp._width,34,26);
         this._em2JoinCode._visible=true; this._em2JoinCode.text="Room code: "+this._em2OnlineRead(2781);
         var joinError=this._em2OnlineRead(2782);
         this._em2OnlineHelp.text=joinError!=""?joinError:"Type the eight-character code from your friend, or choose Paste room code. Enter joins; Esc cancels.";
         this._em2OnlineRefreshTime=getTimer();return;
      }
      if(this._em2JoinCode!=undefined) this._em2JoinCode._visible=false;
      if(this._em2KickConfirm) {
         this._em2OnlineButton(0,"Kick player",true); this._em2OnlineButton(1,"Cancel",true);
         var hide=2; while(hide<6) { this._items[hide].disable(); this._items[hide]._visible=false; this["_em2OnlineInfo"+hide]._visible=false; hide++; }
         this._em2OnlineHelp.text="Remove "+this._em2OnlineRead(2776)+" from this lobby? They can join again with an invitation.";
         this._em2OnlineRefreshTime=getTimer(); return;
      }
      if(this._em2OnlineVoicePage > 0 && state != 7)
      {
         if(this._em2OnlineVoicePage == 1)
         {
            var modes = ["Off","Listen only","Push to talk","Open microphone"];
            var positions = ["Party","Proximity"];
            var ranges = ["10","20","30","50","100"];
            var talkKeys = ["F8","Caps Lock","Alt"];
            this._em2OnlineButton(0,"Voice chat: " + modes[_level0.SetOption(2800,-1)],true);
            this._em2OnlineButton(1,"Voice positioning: " + positions[_level0.SetOption(2804,-1)],true);
            this._em2OnlineButton(2,"Proximity range: " + ranges[_level0.SetOption(2805,-1)] + " units",true);
            this._em2OnlineButton(3,"Push to talk: " + talkKeys[_level0.SetOption(2802,-1)],true);
            this._em2OnlineButton(4,"Microphone / speakers",true);
            this._em2OnlineButton(5,"Back",true);
         }
         else
         {
            this._em2OnlineButton(0,"Microphone: " + this._em2OnlineRead(2808).substr(0,46),true);
            this._em2OnlineButton(1,"Speakers: " + this._em2OnlineRead(2809).substr(0,46),true);
            this._em2OnlineButton(2,"Voice volume: " + (_level0.SetOption(2801,-1)*25) + "%",true);
            this._em2OnlineButton(3,"Refresh audio devices",true);
            this._em2OnlineButton(4,"Back to voice settings",true);
            this._em2OnlineButton(5,"Back",true);
         }
         this._em2OnlineHelp.textColor = 16777215;
         this._em2OnlineHelp.text = this._em2OnlineRead(2803) + "\nMicrophone stays off until enabled. Proximity chat fades with distance during gameplay. Select a setting to change it.";
         this._em2OnlineRefreshTime = getTimer();return;
      }
      if(this._em2OnlineHostPage && !idle) this._em2OnlineHostPage = false;
      if(this._em2OnlineHostPage)
      {
         var archives = _level0.SetOption(2708,-1);
         var archive = _level0.SetOption(2709,-1);
         var scanning = _level0.SetOption(2714,-1) == 1;
         this._em2OnlineButton(0,"Host my save online",true);
         this._em2OnlineButton(1,archives > 0 ? "Continue an online save " + (archive+1) + "/" + archives : "No previous online saves",!scanning && archives > 0);
         this._em2OnlineButton(2,internet ? "Invite only" : (_level0.SetOption(2716,-1) == 1 ? "Visibility: Discoverable on LAN" : "Visibility: Invite only"),!internet);
         this._em2OnlineButton(3,"Next online save",!scanning && archives > 1);
         this._em2OnlineButton(4,scanning ? "Checking online saves..." : "Refresh online saves",!scanning);
         this._em2OnlineButton(5,"Back",true);
         this._em2OnlineHelp.textColor = 16777215;
         this._em2OnlineHelp.text = this._em2OnlineRead(2713);
         this._em2OnlineRefreshTime = getTimer();
         return;
      }
      var count = _level0.SetOption(2703,-1);
      var selected = _level0.SetOption(2704,-1);
      var isHost = _level0.SetOption(2702,-1) == 1;
      var canCopy = isHost && (state == 1 || state == 2 || state == 6) && _level0.SetOption(2711,0) > 0;
      var canStart = state == 6 && isHost && _level0.SetOption(2706,-1) == 1;
      if(state == 6 || (state==2 && isHost && _level0.SetOption(2731,-1)==1))
      {
         var editable = _level0.SetOption(2720,-1) == 1;
         var ready = _level0.SetOption(2721,-1) == 1;
         var room = _level0.SetOption(2731,-1) == 1;
         if(room && this._em2OnlineRosterPage)
         {
            var page = _level0.SetOption(2741,0);
            var pages = _level0.SetOption(2741,1);
            var row = 0;
            while(row < 4)
            {
               var name = this._em2OnlineRead(2738,row*128);
               this._em2OnlineButton(row,name,name != "");
               this._items[row].t_label.textColor = _level0.SetOption(2739,row);
               if(this._list.activeItem == this._items[row]) this._em2OnlineRosterSelected = row;
               row++;
            }
            this._em2OnlineButton(4,"Next players (" + page + "/" + pages + ")",pages > 1);
            this._em2OnlineButton(5,"Back",true);
            this._em2OnlineHelp.textColor = 16777215;
            this._em2OnlineHelp.text = this._em2OnlineRead(2740,this._em2OnlineRosterSelected*128);
            this._em2OnlineRefreshTime = getTimer();
            return;
         }
         if(room && this._em2OnlinePlayerPage)
         {
            this._em2OnlineButton(0,"Name: " + this._em2OnlineRead(2722),editable);
            this._em2OnlineButton(1,"Character: " + (_level0.SetOption(2732,-1) == 0 ? "Mickey" : "Oswald"),_level0.SetOption(2737,-1) == 1);
            this._em2OnlineButton(2,"Pants / name color: " + this._em2OnlineRead(2733),editable);
            this._em2OnlineButton(3,"Voice chat settings",true);
            this._em2OnlineButton(4,"Back",true);
            this._em2OnlineButton(5,"Leave lobby",true);
            this._em2OnlineHelp.textColor = 16777215;
            this._em2OnlineHelp.text = this._em2OnlineRead(2726);
            this._em2OnlineRefreshTime = getTimer();
            return;
         }
         if(room)
         {
            this._em2OnlineButton(0,isHost ? "Start Game" : "Waiting for host",canStart);
            this._em2OnlineButton(1,"Player options",true);
            this._em2OnlineButton(2,isHost ? "Invite Friends" : (ready ? "Ready (select to cancel)" : "Not ready (select when ready)"),isHost ? _level0.SetOption(2745,0) == 1 : editable);
            this._em2OnlineButton(3,isHost?"Manage players":"Player details",true);
            this._em2OnlineButton(4,(_level0.SetOption(2750,-1)&2)!=0?"Send message":"Chat",true);
            this._em2OnlineButton(5,"Leave lobby",true);
            this._em2OnlineHelp.textColor = 16777215;
            this._em2OnlineHelp.text = this._em2OnlineRead(2710)+" "+(isHost?"Invite friends, then start when everyone is ready.":"Choose your character and color, then select Ready.");
            if(isHost && _level0.SetOption(2745,0)!=1) this._em2OnlineHelp.text+=" Open Discord to invite friends, or use the copy icon to share your room code.";
            this._em2OnlineRefreshTime = getTimer();
            return;
         }
         var roster = this._em2OnlineRead(2723);
         this._em2OnlineButton(0,isHost ? (canStart ? "Start online game" : "Waiting for guest readiness") : "Waiting for host",canStart);
         this._em2OnlineButton(1,"Name: " + this._em2OnlineRead(2722),editable);
         this._em2OnlineButton(2,isHost ? "Host" : (ready ? "Ready (select to cancel)" : "Not ready (select when ready)"),editable && !isHost);
         this._em2OnlineButton(3,room ? ("Character: " + (_level0.SetOption(2732,-1) == 0 ? "Mickey" : "Oswald")) : ("Players connected: " + roster.split("\n").length),room && _level0.SetOption(2737,-1) == 1);
         this._em2OnlineButton(4,room ? ("Pants / name color: " + this._em2OnlineRead(2733)) : "Shared campaign",room && editable);
         this._em2OnlineButton(5,"Leave lobby",true);
         var profileMessage = this._em2OnlineRead(2726);
         if(profileMessage == "") profileMessage = this._em2OnlineNameError;
         if(profileMessage == undefined || profileMessage == "") profileMessage = this._em2OnlineRead(2710);
         this._em2OnlineHelp.textColor = 16777215;
         this._em2OnlineHelp.text = roster + "\n" + profileMessage;
         this._em2OnlineRefreshTime = getTimer();
         return;
      }
      this._em2OnlineRestoreRosterColors();
      this._em2OnlinePlayerPage = false;
      this._em2OnlineRosterPage = false;
      var connectedLabel = isHost ? "Session connected" : "Waiting for host";
      var hostLabel = "Host Game";
      if(state == 1) hostLabel = "Checking game files...";
      else if(state == 2) hostLabel = "Lobby open";
      else if(state == 3) hostLabel = "Connecting...";
      else if(state == 4) hostLabel = "Receiving shared save...";
      else if(state == 5) hostLabel = "Preparing shared save...";
      else if(state == 7) hostLabel = "Starting online game...";
      else if(state == 8) hostLabel = "Closing session...";
      this._em2OnlineButton(0,state == 6 ? (canStart ? "Start online game" : connectedLabel) : hostLabel,idle || canStart);
      this._em2OnlineButton(1,internet ? "Join Game" : "Join copied invite",idle);
      this._em2OnlineButton(2,internet ? "Saved online games" : "Find LAN games",idle);
      this._em2OnlineButton(3,count > 0 ? "Join LAN game " + (selected+1) + "/" + count : "No LAN games found",idle && count > 0);
      var inviteLabel = _level0.SetOption(2715,1) == 1 ? "Copy Internet invite" : "Copy LAN invite";
      if(idle) inviteLabel = _level0.SetOption(2715,0) == 1 ? "Auto Internet port: On" : "Auto Internet port: Off";
      this._em2OnlineButton(4,inviteLabel,idle || canCopy);
      if(internet)
      {
         this._em2OnlineButton(3,"Voice chat settings",state != 7);
         if(isHost && !idle) this._em2OnlineButton(2,"Invite Friends",_level0.SetOption(2745,0) == 1);
         this._em2OnlineButton(4,"Copy room code",canCopy);
         if(idle) this.b_4._visible = false;
      }
      this._em2OnlineButton(5,idle ? "Back" : "Cancel session",state != 7);
      var message = this._em2OnlineRead(2710);
      if(idle && internet) message="Host Game opens a room using your current save. Join Game accepts your friend's eight-character room code. You can also accept a Discord invitation.";
      if(state == 1 && internet) message = "Checking game files and preparing your save. The first check can take several minutes. Wait here until your eight-character room code appears.";
      if(idle && count > 0) message += "\nLAN host: " + this._em2OnlineRead(2712) + ". Left / Right selects a host.";
      this._em2OnlineHelp.textColor = state == 9 ? 16755370 : 16777215;
      this._em2OnlineHelp.text = message;
      this._em2OnlineRefreshTime = getTimer();
   }
   function _em2OnlineSelect()
   {
      if(_level0.SetOption(2743,-1) != 1) { this._em2OnlineClose(); return; }
      if(this._em2OnlineEditingName) { this._em2OnlineEndName(true); return; }
      if(this._em2OnlineEditEndedAt != undefined && getTimer() - this._em2OnlineEditEndedAt < 150) return;
      var item = this._list.activeItem; var state = this._em2OnlineState;
      var internet = _level0.SetOption(2743,1) == 1;
      if(this._em2JoinPage) {
         if(item==this.b_main) _level0.SetOption(2780,2);
         else if(item==this.b_1) _level0.SetOption(2780,3);
         else if(item==this.b_2) { _level0.SetOption(2780,0); this._em2JoinPage=false; }
         this._em2OnlineRefresh();return;
      }
      if(this._em2KickConfirm) {
         if(item==this.b_main) _level0.SetOption(2777,1); else _level0.SetOption(2777,0);
         this._em2KickConfirm=false; this._em2OnlineRefresh(); this._list.activate(0); return;
      }
      if(state == 7) return;
      if((state==6 || state==2) && item!=this.b_4) _level0.SetOption(2759,2);
      if(this._em2OnlineVoicePage > 0)
      {
         if(this._em2OnlineVoicePage == 1)
         {
            if(item == this.b_main) _level0.SetOption(2800,(_level0.SetOption(2800,-1)+1)%4);
            else if(item == this.b_1) _level0.SetOption(2804,1-_level0.SetOption(2804,-1));
            else if(item == this.b_2) _level0.SetOption(2805,(_level0.SetOption(2805,-1)+1)%5);
            else if(item == this.b_3) _level0.SetOption(2802,(_level0.SetOption(2802,-1)+1)%3);
            else if(item == this.b_4) this._em2OnlineVoicePage = 2;
            else if(item == this.b_5) this._em2OnlineVoicePage = 0;
         }
         else
         {
            if(item == this.b_main) _level0.SetOption(2806,1);
            else if(item == this.b_1) _level0.SetOption(2807,1);
            else if(item == this.b_2) _level0.SetOption(2801,(_level0.SetOption(2801,-1)+1)%9);
            else if(item == this.b_3) _level0.SetOption(2810,1);
            else if(item == this.b_4) this._em2OnlineVoicePage = 1;
            else if(item == this.b_5) this._em2OnlineVoicePage = 0;
         }
         this._em2OnlineRefresh();return;
      }
      if(state == 6 || (state==2 && _level0.SetOption(2702,-1)==1 && _level0.SetOption(2731,-1)==1))
      {
         if(_level0.SetOption(2731,-1) == 1)
         {
            if(this._em2OnlineRosterPage)
            {
               var selectedRow=0;
               while(selectedRow<4) { if(item==this._items[selectedRow]) { _level0.SetOption(2770,-1); this._em2KickPlayer((_level0.SetOption(2741,0)-1)*4+selectedRow); return; } selectedRow++; }
               if(item == this.b_4) { _level0.SetOption(2742,0); this._em2OnlineRosterSelected = 0; }
               else if(item == this.b_5) { this._em2OnlineBack(); return; }
            }
            else if(this._em2OnlinePlayerPage)
            {
               if(item == this.b_main) { this._em2OnlineBeginName(); return; }
               else if(item == this.b_1) _level0.SetOption(2734,0);
               else if(item == this.b_2) _level0.SetOption(2735,0);
               else if(item == this.b_3) { this._em2OnlineVoicePage = 1; this._em2OnlineRefresh(); this._list.activate(0); return; }
               else if(item == this.b_4) { this._em2OnlineBack(); return; }
               else if(item == this.b_5) _level0.SetOption(2700,6);
            }
            else
            {
               if(item == this.b_main) _level0.SetOption(2700,7);
               else if(item == this.b_1) { this._em2OnlinePlayerPage = true; this._em2OnlineRefresh(); this._list.activate(0); return; }
               else if(item == this.b_2) { if(_level0.SetOption(2702,-1) == 1) _level0.SetOption(2745,1); else _level0.SetOption(2725,0); }
               else if(item == this.b_3) { this._em2OnlineShowRoster(false); return; }
               else if(item == this.b_4) _level0.SetOption(2759,(_level0.SetOption(2750,-1)&2)!=0?1:0);
               else if(item == this.b_5) _level0.SetOption(2700,6);
            }
            this._em2OnlineRefresh(); return;
         }
         if(item == this.b_main) _level0.SetOption(2700,7);
         else if(item == this.b_1) { this._em2OnlineBeginName(); return; }
         else if(item == this.b_2) _level0.SetOption(2725,0);
         else if(item == this.b_3) _level0.SetOption(2734,0);
         else if(item == this.b_4) _level0.SetOption(2735,0);
         else if(item == this.b_5) _level0.SetOption(2700,6);
         this._em2OnlineRefresh(); return;
      }
      if(this._em2OnlineHostPage)
      {
         if(item == this.b_main) { this._em2OnlineHostPage = false; _level0.SetOption(2700,1); }
         else if(item == this.b_1) _level0.SetOption(2700,12);
         else if(item == this.b_2) _level0.SetOption(2700,16);
         else if(item == this.b_3) _level0.SetOption(2700,14);
         else if(item == this.b_4) _level0.SetOption(2700,11);
         else if(item == this.b_5) this._em2OnlineHostPage = false;
         this._em2OnlineRefresh(); return;
      }
      if(item == this.b_main)
      {
         if(state == 6) _level0.SetOption(2700,7);
         else if(internet) _level0.SetOption(2700,1);
         else { this._em2OnlineHostPage = true; _level0.SetOption(2700,11); }
      }
      else if(item == this.b_1) {
         if(internet) { this._em2JoinPage=true; _level0.SetOption(2780,1); this._em2OnlineRefresh(); this._list.activate(0); return; }
         _level0.SetOption(2700,2);
      }
      else if(item == this.b_2)
      {
         if(internet && state != 0 && state != 9) _level0.SetOption(2745,1);
         else if(internet) { this._em2OnlineHostPage = true; _level0.SetOption(2700,11); }
         else _level0.SetOption(2700,3);
      }
      else if(item == this.b_3) { if(internet) this._em2OnlineVoicePage = 1; else _level0.SetOption(2700,4); }
      else if(item == this.b_4) _level0.SetOption(2700,state == 0 || state == 9 ? 15 : 5);
      else if(item == this.b_5)
      {
         if(state == 0 || state == 9) { this._em2OnlineClose(); return; }
         _level0.SetOption(2700,6);
      }
      this._em2OnlineRefresh();
   }
   function _em2OnlineTick()
   {
      if(!this._em2Online) return;
      if(_level0.SetOption(2743,-1) != 1) { this._em2OnlineClose(); return; }
      var state = _level0.SetOption(2701,-1);
      if(this._em2OnlineEditingName)
      {
         if(state != 6 || _level0.SetOption(2727,-1) != 1) this._em2OnlineEndName(false);
         else
         {
            this._em2OnlineNameInput.text = this._em2OnlineRead(2728);
            Selection.setSelection(_level0.SetOption(2730,-1),_level0.SetOption(2729,-1));
         }
      }
      // Diagnostic automation still calls the real native page/controller.
      // Its role is nonzero only for validated private loopback test roots.
      var role = _level0.SetOption(2707,-1);
      if(role > 0 && !this._em2OnlineQaIssued && state == 0)
      {
         if(role == 1) { _level0.SetOption(2700,1); this._em2OnlineQaIssued = true; }
         else
         {
            _level0.SetOption(2700,10);
            if(_level0.SetOption(2701,-1) != 0) this._em2OnlineQaIssued = true;
         }
         state = _level0.SetOption(2701,-1);
      }
      if(role == 1 && state == 6 && _level0.SetOption(2706,-1) == 1)
      {
         _level0.SetOption(2700,7);
         state = _level0.SetOption(2701,-1);
      }
      if(this._em2JoinPage && this._em2JoinCode!=undefined) this._em2JoinCode.text="Room code: "+this._em2OnlineRead(2781);
      if(this._em2LobbyMode && this._em2LobbyPanel!=undefined) {
         var draftRevision=_level0.SetOption(2758,2);
         if(draftRevision!=this._em2ChatDraftRevision) {
            this._em2ChatDraftRevision=draftRevision;
            var editing=(_level0.SetOption(2750,-1)&2)!=0;
            this._em2LobbyPanel.chatDock.draft.text=editing?this._em2OnlineRead(2757):"Click here or press T to chat";
            if(this._em2LobbyPanel.keyboard._visible) this._em2LobbyPanel.keyboard.text=this._em2OnlineRead(2762)+"\nA: type   X: delete   Y: send   B: close";
         }
      }
      if(state != this._em2OnlineState || getTimer() - this._em2OnlineRefreshTime > 200) this._em2OnlineRefresh();
   }
   function _em2OnlineBeginName()
   {
      if(_level0.SetOption(2720,-1) != 1 || _level0.SetOption(2727,1) != 1) return;
      this._em2OnlineNameError = "";
      if(this._em2OnlineNameInput == undefined)
      {
         this.createTextField("_em2OnlineNameInput",this.getNextHighestDepth(),this._em2OnlineHelp._x,this._em2OnlineHelp._y,this._em2OnlineHelp._width,32);
         var format = this._em2OnlineHelp.getNewTextFormat(); format.align = "left";
         this._em2OnlineNameInput.embedFonts = false;
         this._em2OnlineNameInput.setNewTextFormat(format);
         this._em2OnlineNameInput.type = "input";
         this._em2OnlineNameInput.selectable = true;
         this._em2OnlineNameInput.maxChars = 24;
         this._em2OnlineNameInput.border = true;
         this._em2OnlineNameInput.background = true;
         this._em2OnlineNameInput.backgroundColor = 1710618;
         this._em2OnlineNameInput.textColor = 16777215;
      }
      if(this._em2OnlineNameHint == undefined)
      {
         this.createTextField("_em2OnlineNameHint",this.getNextHighestDepth(),this._em2OnlineHelp._x,this._em2OnlineHelp._y+36,this._em2OnlineHelp._width,32);
         var hintFormat = this._em2OnlineHelp.getNewTextFormat(); hintFormat.size = 14; hintFormat.align = "center";
         this._em2OnlineNameHint.embedFonts = false;
         this._em2OnlineNameHint.setNewTextFormat(hintFormat);
         this._em2OnlineNameHint.selectable = false;
         this._em2OnlineNameHint.textColor = 16777215;
         this._em2OnlineNameHint.text = "Type your name. Enter / A saves. Esc / B cancels.";
      }
      this._em2OnlineNameHint._visible = true;
      this._em2OnlineNameInput.text = this._em2OnlineRead(2722);
      this._em2OnlineNameInput._visible = true;
      this._em2OnlineHelp._visible = false;
      this._em2OnlineEditingName = true;
      this._list.deactivate();
      Selection.setFocus(this._em2OnlineNameInput);
      Selection.setSelection(0,this._em2OnlineNameInput.text.length);
   }
   function _em2OnlineEndName(commit)
   {
      if(!this._em2OnlineEditingName) return;
      this._em2OnlineEditingName = false;
      this._em2OnlineEditEndedAt = getTimer();
      if(commit) _level0.SetOption(2727,2);
      else _level0.SetOption(2727,0);
      this._em2OnlineNameInput._visible = false;
      this._em2OnlineNameHint._visible = false;
      this._em2OnlineHelp._visible = true;
      Selection.setFocus(null);
      this._list.activate(this._em2OnlinePlayerPage ? 0 : 1);
      this._em2OnlineRefresh();
   }
   function _em2OnlineKeyDown()
   {
      if(this._em2OnlineEditingName)
      {
         var editKey = Key.getCode();
         if(editKey == 13) this._em2OnlineEndName(true);
         else if(editKey == 27) this._em2OnlineEndName(false);
         return;
      }
      if(!this._em2Online) return;
      var key = Key.getCode();
      if(key==37 || key==38 || key==39 || key==40) this._em2MoveSelection(key==37?-1:key==39?1:0,key==38?-1:key==40?1:0);
   }
   function _em2OnlineShowRoster(fromPlayerPage)
   {
      var i = 0;
      while(i < 4) { this._items[i]._em2OnlineSavedTextColor = this._items[i].t_label.textColor; i++; }
      this._em2OnlineRosterReturnPlayer = fromPlayerPage;
      this._em2OnlineRosterSelected = 0;
      this._em2OnlineRosterPage = true;
      _level0.SetOption(2742,2);
      this._em2OnlineRefresh(); this._list.activate(0);
   }
   function _em2OnlineBack()
   {
      if(this._em2JoinPage) { _level0.SetOption(2780,0); this._em2JoinPage=false; this._em2OnlineRefresh(); this._list.activate(1); return; }
      if(this._em2KickConfirm) { _level0.SetOption(2777,0); this._em2KickConfirm=false; this._em2OnlineRefresh(); this._list.activate(3); return; }
      if(this._em2OnlineEditingName) { this._em2OnlineEndName(false); return; }
      if(this._em2OnlineVoicePage > 0) { this._em2OnlineVoicePage = this._em2OnlineVoicePage == 2 ? 1 : 0; this._em2OnlineRefresh(); this._list.activate(0); return; }
      if(this._em2OnlineRosterPage) { this._em2OnlineRestoreRosterColors(); this._em2OnlineRosterPage = false; this._em2OnlinePlayerPage = this._em2OnlineRosterReturnPlayer; this._em2OnlineRefresh(); this._list.activate(3); return; }
      if(this._em2OnlinePlayerPage) { this._em2OnlinePlayerPage = false; this._em2OnlineRefresh(); this._list.activate(1); return; }
      if(this._em2OnlineHostPage) { this._em2OnlineHostPage = false; this._em2OnlineRefresh(); return; }
      if(this._em2OnlineState != 7) this._em2OnlineClose();
   }
   function _em2OnlineRestoreRosterColors()
   {
      var i = 0;
      while(i < 4)
      {
         var button = this._items[i];
         if(button._em2OnlineSavedTextColor != undefined) { button.t_label.textColor = button._em2OnlineSavedTextColor; delete button._em2OnlineSavedTextColor; }
         i++;
      }
   }
