      // Private native-lobby QA follows the original No-web branch after
      // accepting the original first-use notice, instead of contacting the
      // obsolete Disney web service or creating a fake campaign.
      if(!savegame.isSavedGame && _level0.SetOption != undefined && _level0.SetOption(2501,3) == 1)
      {
         _level0.SetWebEnabled(false);
         this._goToMainMenu();
         return;
      }
