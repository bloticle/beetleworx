// Methods inserted into the original HUD class by build-pc-menu.py.
var _pcSelectedCharacter = false;
function set pcSelectedCharacter(value)
{
   this._pcSelectedCharacter = value == 1;
   this._setGeometry();
   this.p1cursor._x = this._screenCenter;
   this.p1cursor._y = _global.initVars.screenHeight >> 1;
   this.notifications._y = _global.initVars.screenHeight - _global.offsetHeight;
   this._updatePlayerHUDVisibility();
}

function _showSelectedCharacterHUD()
{
   var showing = this._isShowing && !this._cutSceneActive;
   this.mickey._visible = false;
   this.oswald._visible = showing && !this._oswaldCameraEnabled;
   this.p1cursor._visible = showing && !this._isMinimalMode && !this._oswaldCameraEnabled;
   this.p2cursor._visible = false;
}
