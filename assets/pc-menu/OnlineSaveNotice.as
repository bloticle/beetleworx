      this._em2OnlineNoticeTrace(4);
      // Only the original Xbox/PS3 existing-save information screen may
      // auto-confirm normally. First-use QA has a separate authorization below;
      // normal new/empty saves and reset/delete dialogs keep native flow.
      if(_level0.IsGen3() && this._showingSavingMessage && _level0.IsSavedGamePresent() && _level0.SetOption != undefined && _level0.SetOption(2501,1) == 1)
      {
         this._acceptSavingMessage();
      }
      // Only explicit private loopback QA may accept this first-use notice
      // without an existing campaign. Normal/new-save flows remain authored.
      else if(_level0.IsGen3() && this._showingSavingMessage && !_level0.IsSavedGamePresent() && _level0.SetOption != undefined && _level0.SetOption(2501,2) == 1)
      {
         this._acceptSavingMessage();
      }
