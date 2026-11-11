   function _em2OnlineNoticeTrace(stage)
   {
      if(_level0.SetOption != undefined)
      {
         var flags = (_level0.IsGen3() ? 1 : 0) | (this.tabEnabled ? 2 : 0) | (this._activeSlot.isSavedGame ? 4 : 0) | (this._showingSavingMessage ? 8 : 0);
         _level0.SetOption(2501,stage * 100 + flags);
      }
   }
