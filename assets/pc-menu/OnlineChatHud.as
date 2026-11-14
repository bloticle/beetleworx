// HUD-owned chat. All strings are assigned as plain text, never HTML or code.
function _pcChatRead(id)
{
   var escaped = ""; var hex = "0123456789ABCDEF"; var i = 0;
   while(i < 400)
   {
      var value = _level0.PCOnlineChat(id,i);
      if(value == undefined || value <= 0) break;
      escaped += "%" + hex.charAt(value >> 4) + hex.charAt(value & 15); i++;
   }
   try { return decodeURIComponent(escaped); } catch(error) { return ""; }
}
function _pcChatField(clip,name,y,height,size)
{
   clip.createTextField(name,clip.getNextHighestDepth(),12,y,this._pcChatWidth-24,height);
   var field = clip[name]; var format = this.cutsceneskip.t_label.getTextFormat();
   format.size = size; format.align = "left"; format.color = 16777215;
   field.embedFonts = true; field.setNewTextFormat(format);
   field.multiline = true; field.wordWrap = true; field.selectable = false;
   return field;
}
function _pcChatInit()
{
   this._pcChatClock = 0;
   this._pcChatPump = this.createEmptyMovieClip("_pcChatPump",this.getNextHighestDepth());
   this._pcChatPump.onEnterFrame = function() { this._parent._pcChatTick(); };
}
function _pcChatTick()
{
   if(getTimer()-this._pcChatClock < 100) return;
   this._pcChatClock = getTimer();
   if(_level0.PCOnlineChat == undefined) return;
   if(this._pcRecoveryTick()) return;
   // Original property subscriptions can run before extra humans are admitted.
   // Query the actual local actor through the HUD bridge, then drive the real
   // authored status setter only on changes. This also covers later damage.
   var mickeyLife = _level0.PCOnlineChat(2764,-1);
   var oswaldLife = _level0.PCOnlineChat(2765,-1);
   if(mickeyLife >= 0)
   {
      if(mickeyLife != this._pcOnlineMickeyLife) this.mickey.lifeChanged = mickeyLife;
      this._pcOnlineMickeyLife = mickeyLife;
   }
   else this._pcOnlineMickeyLife = undefined;
   if(oswaldLife >= 0)
   {
      if(oswaldLife != this._pcOnlineOswaldLife) this.oswald.lifeChanged = oswaldLife;
      this._pcOnlineOswaldLife = oswaldLife;
   }
   else this._pcOnlineOswaldLife = undefined;
   var flags = _level0.PCOnlineChat(2750,-1);
   if(this._pcChatPanel == undefined)
   {
      this._pcChatWidth = Math.min(520,_global.initVars.screenWidth*0.48);
      var panel = this.createEmptyMovieClip("_pcChatPanel",this.getNextHighestDepth());
      panel._x = _global.offsetX + 24;
      panel.beginFill(0,75); panel.moveTo(0,0); panel.lineTo(this._pcChatWidth,0);
      panel.lineTo(this._pcChatWidth,516); panel.lineTo(0,516); panel.lineTo(0,0); panel.endFill();
      this._pcChatField(panel,"title",8,24,18).text = "Chat";
      var i = 0; while(i < 4)
      {
         this._pcChatField(panel,"name"+i,36+i*68,20,16);
         this._pcChatField(panel,"body"+i,56+i*68,48,16); i++;
      }
      this._pcChatField(panel,"draft",312,60,16);
      this._pcChatField(panel,"keyboard",376,88,14);
      this._pcChatField(panel,"help",468,44,14);
      _level0.PCOnlineChat(2761,1);
   }
   var panel = this._pcChatPanel; var edit = (flags & 2) != 0;
   panel._visible = (flags & 4) != 0 && (edit || !this._cutSceneActive);
   if(!panel._visible) return;
   panel._y = _global.initVars.screenHeight - _global.offsetHeight - 24 - (edit ? 516 : 360);
   panel.keyboard._visible = edit; panel.draft._visible = edit;
   var version = _level0.PCOnlineChat(2751,-1);
   if(version != this._pcChatVersion)
   {
      this._pcChatVersion = version;
      var count = _level0.PCOnlineChat(2752,-1); var start = Math.max(0,count-4); var i = 0;
      while(i < 4)
      {
         var present = start+i < count && _level0.PCOnlineChat(2753,start+i) == 1;
         panel["name"+i].text = present ? this._pcChatRead(2754) : "";
         panel["body"+i].text = present ? this._pcChatRead(2755) : "";
         panel["name"+i].textColor = present ? _level0.PCOnlineChat(2756,-1) : 16777215;
         i++;
      }
      _level0.PCOnlineChat(2761,3);
   }
   if(edit)
   {
      var draftVersion = _level0.PCOnlineChat(2758,2);
      if(draftVersion != this._pcChatDraftVersion)
      {
         this._pcChatDraftVersion = draftVersion;
         var draft = this._pcChatRead(2757); var cursor = _level0.PCOnlineChat(2758,0);
         panel.draft.text = "> " + draft.substring(0,cursor) + "|" + draft.substring(cursor);
         panel.keyboard.text = this._pcChatRead(2762);
         _level0.PCOnlineChat(2761,2);
      }
      var feedback = this._pcChatRead(2760);
      panel.help.text = feedback != "" ? feedback : "Enter / Y: Send   Escape / B: Close\nD-pad: Select   A: Type   X: Delete";
   }
   else panel.help.text = "T / D-pad Up: Chat";
   // Keep the closed background within its content, without scaling text.
   panel.clear(); panel.beginFill(0,edit ? 80 : 55); panel.moveTo(0,0);
   panel.lineTo(this._pcChatWidth,0); panel.lineTo(this._pcChatWidth,edit ? 516 : 360);
   panel.lineTo(0,edit ? 516 : 360); panel.lineTo(0,0); panel.endFill();
   panel.help._y = edit ? 468 : 316;
}
function _pcRecoveryTick()
{
   var state = _level0.PCOnlineChat(2790,-1);
   if(state == undefined || state <= 0)
   {
      if(this._pcRecoveryPanel != undefined) this._pcRecoveryPanel._visible = false;
      return false;
   }
   if(this._pcChatPanel != undefined) this._pcChatPanel._visible = false;
   if(this._pcRecoveryPanel == undefined)
   {
      var panel = this.createEmptyMovieClip("_pcRecoveryPanel",this.getNextHighestDepth());
      var width = Math.min(560,_global.initVars.screenWidth*0.8);
      panel._x = _global.offsetX + (_global.initVars.screenWidth-width)*0.5;
      panel._y = _global.initVars.screenHeight*0.3;
      panel.beginFill(0,90);panel.moveTo(0,0);panel.lineTo(width,0);panel.lineTo(width,190);panel.lineTo(0,190);panel.endFill();
      panel.createTextField("message",1,18,14,width-36,110);
      var format = this.cutsceneskip.t_label.getTextFormat();format.size=18;format.align="center";format.color=16777215;
      panel.message.embedFonts=true;panel.message.setNewTextFormat(format);
      panel.message.multiline=true;panel.message.wordWrap=true;panel.message.selectable=false;
      var button=panel.createEmptyMovieClip("back",2);
      button._x=24;button._y=132;button.beginFill(2631720,100);button.moveTo(0,0);button.lineTo(width-48,0);button.lineTo(width-48,42);button.lineTo(0,42);button.endFill();
      button.createTextField("label",1,4,9,width-56,28);button.label.embedFonts=true;button.label.setNewTextFormat(format);button.label.selectable=false;
      button.onRelease=function(){if(this.enabled)_level0.PCOnlineChat(2790,1);};
   }
   var panel=this._pcRecoveryPanel;panel._visible=true;
   panel.message.text=this._pcChatRead(2791);
   panel.back.enabled=state==1;panel.back._alpha=state==1?100:55;
   panel.back.label.text=state==1?"Return to title (Enter)":state==4?"Restart required":"Returning to title...";
   return true;
}
