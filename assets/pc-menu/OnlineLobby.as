   function _em2OnlineRead(id)
   {
      var text = ""; var escaped = ""; var hex = "0123456789ABCDEF"; var i = 0;
      while(i < 400)
      {
         var value = _level0.SetOption(id,i);
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
      button._visible = true; button._alpha = 100; button.label = label;
      if(enabled) button.enable(); else button.disable();
   }
   function _em2OnlineOpen()
   {
      if(_level0.SetOption(7203,-1) == 1) { this.b_5.disable(); this.b_5._visible = false; return; }
      this._em2Online = true;
      this._em2OnlineHostPage = false;
      _level0.SetOption(2700,0);
      if(this._em2OnlineHelp == undefined)
      {
         var width = _global.initVars.screenWidth;
         this.createTextField("_em2OnlineHelp",this.getNextHighestDepth(),_global.offsetX + width*0.1,this.b_select._y - 86,width*0.8,76);
         var format = this.b_main.t_label.getTextFormat(); format.size = 16; format.align = "center";
         this._em2OnlineHelp.embedFonts = true; this._em2OnlineHelp.setNewTextFormat(format);
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
      _level0.SetOption(2700,6);
      this._em2Online = false;
      if(this._em2OnlinePump != undefined) this._em2OnlinePump.removeMovieClip();
      Key.removeListener(this._em2OnlineKeyListener);
      if(this._em2OnlineHelp != undefined) this._em2OnlineHelp._visible = false;
      this._setText();
      var i = 0; while(i < this._items.length) { this._items[i].enable(); this._items[i]._visible = true; i++; }
      if(!_level0.IsSavedGamePresent()) { this.b_main.disable(); this.b_main._visible = false; }
      if(_level0.SetOption(7203,-1) == 1) { this.b_5.disable(); this.b_5._visible = false; this._list.activate(0); }
      else this._list.activate(5);
   }
   function _em2OnlineRefresh()
   {
      if(!this._em2Online) return;
      if(_level0.SetOption(7203,-1) == 1) { this._em2OnlineClose(); return; }
      var state = _level0.SetOption(2701,-1);
      this._em2OnlineState = state;
      var idle = state == 0 || state == 9;
      if(this._em2OnlineHostPage && !idle) this._em2OnlineHostPage = false;
      if(this._em2OnlineHostPage)
      {
         var archives = _level0.SetOption(2708,-1);
         var archive = _level0.SetOption(2709,-1);
         var scanning = _level0.SetOption(2714,-1) == 1;
         this._em2OnlineButton(0,"Host offline campaign",true);
         this._em2OnlineButton(1,archives > 0 ? "Resume online campaign " + (archive+1) + "/" + archives : "No online campaigns yet",!scanning && archives > 0);
         this._em2OnlineButton(2,"Previous campaign",!scanning && archives > 1);
         this._em2OnlineButton(3,"Next campaign",!scanning && archives > 1);
         this._em2OnlineButton(4,scanning ? "Checking campaigns..." : "Refresh campaigns",!scanning);
         this._em2OnlineButton(5,"Back",true);
         this._em2OnlineHelp.textColor = 16777215;
         this._em2OnlineHelp.text = this._em2OnlineRead(2713);
         this._em2OnlineRefreshTime = getTimer();
         return;
      }
      var count = _level0.SetOption(2703,-1);
      var selected = _level0.SetOption(2704,-1);
      var isHost = _level0.SetOption(2702,-1) == 1;
      var canCopy = (state == 1 || state == 2) && isHost;
      var canStart = state == 6 && isHost && _level0.SetOption(2706,-1) == 1;
      var connectedLabel = isHost ? "Session connected" : "Waiting for host";
      this._em2OnlineButton(0,state == 6 ? (canStart ? "Start online game" : connectedLabel) : (state == 7 ? "Preparing game..." : "Host game"),idle || canStart);
      this._em2OnlineButton(1,"Join copied invite",idle);
      this._em2OnlineButton(2,"Find LAN games",idle);
      this._em2OnlineButton(3,count > 0 ? "Join LAN game " + (selected+1) + "/" + count : "No LAN games found",idle && count > 0);
      var inviteLabel = _level0.SetOption(2715,1) == 1 ? "Copy Internet invite" : "Copy LAN invite";
      if(idle) inviteLabel = _level0.SetOption(2715,0) == 1 ? "Auto Internet port: On" : "Auto Internet port: Off";
      this._em2OnlineButton(4,inviteLabel,idle || canCopy);
      this._em2OnlineButton(5,idle ? "Back" : "Cancel session",state != 7);
      var message = this._em2OnlineRead(2710);
      if(idle && count > 0) message += "\nLAN host: " + this._em2OnlineRead(2712) + ". Left / Right selects a host.";
      this._em2OnlineHelp.textColor = state == 9 ? 16755370 : 16777215;
      this._em2OnlineHelp.text = message;
      this._em2OnlineRefreshTime = getTimer();
   }
   function _em2OnlineSelect()
   {
      if(_level0.SetOption(7203,-1) == 1) { this._em2OnlineClose(); return; }
      var item = this._list.activeItem; var state = this._em2OnlineState;
      if(state == 7) return;
      if(this._em2OnlineHostPage)
      {
         if(item == this.b_main) { this._em2OnlineHostPage = false; _level0.SetOption(2700,1); }
         else if(item == this.b_1) _level0.SetOption(2700,12);
         else if(item == this.b_2) _level0.SetOption(2700,13);
         else if(item == this.b_3) _level0.SetOption(2700,14);
         else if(item == this.b_4) _level0.SetOption(2700,11);
         else if(item == this.b_5) this._em2OnlineHostPage = false;
         this._em2OnlineRefresh(); return;
      }
      if(item == this.b_main)
      {
         if(state == 6) _level0.SetOption(2700,7);
         else { this._em2OnlineHostPage = true; _level0.SetOption(2700,11); }
      }
      else if(item == this.b_1) _level0.SetOption(2700,2);
      else if(item == this.b_2) _level0.SetOption(2700,3);
      else if(item == this.b_3) _level0.SetOption(2700,4);
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
      if(_level0.SetOption(7203,-1) == 1) { this._em2OnlineClose(); return; }
      var state = _level0.SetOption(2701,-1);
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
      if(state != this._em2OnlineState || getTimer() - this._em2OnlineRefreshTime > 200) this._em2OnlineRefresh();
   }
   function _em2OnlineKeyDown()
   {
      if(!this._em2Online || !(this._em2OnlineState == 0 || this._em2OnlineState == 9)) return;
      var key = Key.getCode();
      if(key == 37 || key == 39) { _level0.SetOption(2700,this._em2OnlineHostPage ? (key == 39 ? 14 : 13) : (key == 39 ? 8 : 9)); this._em2OnlineRefresh(); }
   }
