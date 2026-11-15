// Methods inserted into the original HUD class by build-pc-menu.py.
var _pcSelectedCharacter = false;
var _pcHudMode = 0;
var _pcHudOswald = false;
function set pcSelectedCharacter(value)
{
   this._pcHudMode = value;
   this._pcSelectedCharacter = value == 1 || value == 2 || value == 3;
   this._pcHudOswald = value == 1 || value == 3;
   this._setGeometry();
   this.p1cursor._x = this._screenCenter;
   this.p1cursor._y = _global.initVars.screenHeight >> 1;
   this.p2cursor._x = this._screenCenter;
   this.p2cursor._y = _global.initVars.screenHeight >> 1;
   this.notifications._y = _global.initVars.screenHeight - _global.offsetHeight;
   this._updatePlayerHUDVisibility();
}

function _showSelectedCharacterHUD()
{
   var showing = this._isShowing && !this._cutSceneActive;
   var cameraEnabled = this._pcHudOswald ? this._oswaldCameraEnabled : this._mickeyCameraEnabled;
   this.mickey._visible = showing && !this._pcHudOswald && !cameraEnabled;
   this.oswald._visible = showing && this._pcHudOswald && !cameraEnabled;
   var cursor = (showing && !this._isMinimalMode && !cameraEnabled) || (!this._cutSceneActive && this._useCursorOnly);
   this.p1cursor._visible = cursor && this._pcHudMode != 3;
   this.p2cursor._visible = cursor && this._pcHudMode == 3;
   this.mickeyCameraReticle._visible = showing && !this._pcHudOswald && cameraEnabled;
   this.oswaldCameraReticle._visible = showing && this._pcHudOswald && cameraEnabled;
   if(showing && cameraEnabled)
   {
      var reticle = this._pcHudOswald ? this.oswaldCameraReticle : this.mickeyCameraReticle;
      reticle._x = this._screenCenter;
      reticle.show();
   }
}
