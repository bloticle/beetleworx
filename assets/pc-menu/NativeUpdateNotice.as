   function _pcUpdateDialog()
   {
      var dialog=this._parent.dialog;
      if(dialog==undefined || dialog.summon==undefined) return undefined;
      if(dialog._pcMenuInput==undefined) {
         dialog._pcMenuInput=this._pcMenuInput; dialog._pcPrompt=this._pcPrompt;
         dialog.b_select=dialog.b_confirm; dialog.b_back=dialog.b_cancel;
         dialog._list={_active:true,_items:[]};
         dialog._goBack=function() {
            if(this._callbackWhenCancelling!=undefined) this._callbackWhenCancelling(this._callbackWhenCancellingData);
            this.hide();
         };
      }
      dialog._list._active=dialog.isActive && (dialog._activeData==undefined || dialog.t_title.text==dialog._activeData.title);
      return dialog;
   }
   function _pcUpdateAccept()
   {
      this._pcUpdateAccepted=true; this._pcUpdateBackground=false;
      _level0.SetOption(7316,1);
   }
   function _pcUpdateLater() { _level0.SetOption(7316,0); }
   function _pcUpdateKeepPlaying() { this._pcUpdateBackground=true; }
   function _pcUpdateTick()
   {
      var dialog=this._pcUpdateDialog();
      if(dialog==undefined || _level0.SetOption==undefined) return;
      if(!this._pcUpdateOffered && _level0.SetOption(7315,-1)==1 && !dialog.isActive && this._parent._visible!=false) {
         this._pcUpdateOffered=true;
         dialog.summon({title:"Beetleworx update available",message:"Version "+this._em2OnlineRead(7313)+" is available. Would you like to update? Your saves, settings and game files will be kept.",confirm:"Update",cancel:"Not now",callback:mx.utils.Delegate.create(this,this._pcUpdateAccept),callbackWhenCancelling:mx.utils.Delegate.create(this,this._pcUpdateLater)});
         return;
      }
      if(!this._pcUpdateAccepted || this._pcUpdateBackground || this._pcUpdateRestarted) return;
      var state=_level0.SetOption(7310,-1);
      if(state==4) {
         if(dialog.isActive && dialog.t_title.text!="Updating Beetleworx") return;
         if(dialog.isActive) dialog.hide();
         this._pcUpdateRestarted=true; _level0.SetOption(7318,1); return;
      }
      if(state==6) {
         if(dialog.isActive && dialog.t_title.text!="Updating Beetleworx") return;
         if(dialog.isActive) dialog.hide();
         this._pcUpdateAccepted=false;
         dialog.summon({title:"Update could not finish",message:this._em2OnlineRead(7319)+" Your installation has not been changed. You can try again from Support & Updates.",confirm:"OK"});
         return;
      }
      if(!dialog.isActive) dialog.summon({title:"Updating Beetleworx",message:"Downloading and verifying the update. Beetleworx will restart when it is ready. You can keep playing instead; the update will then install when you exit normally.",cancel:"Keep playing",callbackWhenCancelling:mx.utils.Delegate.create(this,this._pcUpdateKeepPlaying)});
   }
