class em2.options.PlayerControls extends MovieClip
{
   var _list; var _rows; var _definitions; var _actions;
   var _enabled = false; var _capturing = false; var _view = 1; var _refreshTick = 0;
   var _category = 5; var _page = 0; var _profile = 0; var _selected = 0;
   var slider_brightness; var b_threeDSettings; var t_bc_body; var t_bc_title;
   var t_sr_body; var t_sr_title; var t_c1; var t_title;
   function PlayerControls() { super(); }
   function init()
   {
      this.slider_brightness._visible = false; this.b_threeDSettings._visible = false;
      this.t_bc_body._visible = false; this.t_bc_title._visible = false;
      this.t_sr_body._visible = false; this.t_sr_title._visible = false;
      this.t_c1._visible = false; this.t_title._visible = false;
      this._definitions = [{title: "Display & Performance", options: [{id: 1000, title: "Display resolution", choices: ["1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"]}, {id: 1001, title: "Window mode", choices: ["Windowed", "Borderless fullscreen"]}, {id: 1002, title: "Frame limit", choices: ["30 FPS", "40 FPS", "50 FPS", "60 FPS", "90 FPS", "120 FPS", "144 FPS", "165 FPS", "240 FPS", "Uncapped"]}, {id: 1003, title: "3D render resolution", choices: ["720p (1x pixels)", "1440p (4x pixels)", "2160p (9x pixels)"]}, {id: 1026, title: "Screen upscaler", choices: ["Bilinear", "CAS (SDR/HDR)", "FSR 1 (SDR/HDR)"]}, {id: 1027, title: "CAS / FSR sharpness", choices: ["Soft", "Balanced", "Sharp"]}, {id: 1011, title: "Variable refresh / tearing", choices: ["Disabled", "Allowed"]}]}, {title: "Rendering & Lighting", options: [{id: 1004, title: "Player shadows", choices: ["128", "256", "512", "1024", "2048"]}, {id: 1005, title: "Environment shadows", choices: ["512", "1024", "2048", "4096 (native cap: 2048)"]}, {id: 1017, title: "Shadow sampling", choices: ["Original (4 taps)", "8 taps (experimental)", "16 taps (experimental)", "Adaptive (20 reads, exp.)"]}, {id: 1028, title: "Distance shadow resolution", choices: ["Fixed", "Quality", "Balanced", "Performance"]}, {id: 1018, title: "Bloom", choices: ["Off (experimental)", "Original sampling", "Smooth sampling"]}, {id: 1025, title: "Scene lighting (experimental)", choices: ["Original", "Material AO", "AO + diffuse bounce"]}, {id: 1006, title: "Texture cache budget", choices: ["256 MB", "512 MB", "768 MB", "1024 MB", "2048 MB"]}, {id: 1007, title: "Stable lighting workaround", choices: ["Off", "On"]}, {id: 1008, title: "Legacy texture bindings", choices: ["Off", "On"]}, {id: 1009, title: "Performance display", choices: ["Off", "On"]}, {id: 1010, title: "Texture cache lifetime", choices: ["5 seconds", "15 seconds", "30 seconds", "60 seconds", "120 seconds"]}, {id: 1012, title: "Anisotropic filtering", choices: ["Original", "2x", "4x", "8x", "16x"]}, {id: 1013, title: "Lighting ray casts", choices: ["Off", "On"]}, {id: 1014, title: "Lighting update detail", choices: ["Original", "High", "Ultra"]}, {id: 1038, title: "Gremlin dialogue names", choices: ["Retail color names", "Original character names"]}, {id: 1039, title: "Mickey ink drips (requires EM1)", choices: ["Off", "On"]}, {id: 1989, title: "Original Mickey ink assets", readOnly: true, choices: ["Not loaded - requires mounted EM1 assets", "Loaded"]}]}, {title: "HDR Calibration", options: [{id: 1021, title: "HDR output (experimental)", choices: ["Off", "On (Windows HDR required)"]}, {id: 1022, title: "HDR paper white", choices: ["80 nits", "120 nits", "160 nits", "200 nits", "250 nits", "300 nits", "400 nits"]}, {id: 1023, title: "HDR display peak", choices: ["400 nits", "600 nits", "800 nits", "1000 nits", "1500 nits", "2000 nits", "4000 nits"]}, {id: 1024, title: "HDR scene exposure", choices: ["-2.0 EV", "-1.0 EV", "-0.5 EV", "0 EV", "+0.5 EV", "+1.0 EV", "+2.0 EV"]}]}, {title: "Camera", options: [{id: 1016, title: "Offline 3D view width (FOV)", choices: ["Original", "110%", "125%", "150%", "175%"]}, {id: 1019, title: "Offline 3D camera response", choices: ["Original smoothing", "Faster smoothing", "No input smoothing"]}, {id: 1020, title: "Offline mouse camera", choices: ["Off", "Raw movement (experimental)"]}, {id: 1029, title: "Cursor on controller-to-mouse switch", choices: ["Keep position", "Center"]}, {id: 1030, title: "Cursor when starting paint / aim", choices: ["Keep position", "Center"]}, {id: 1031, title: "Confine mouse to game window", choices: ["Off", "On"]}, {id: 1032, title: "Hide desktop cursor during mouse camera", choices: ["Off", "On"]}, {id: 1033, title: "Camera input priority", choices: ["Last active device", "Always allow mouse"]}, {id: 1034, title: "Mouse takeover threshold", choices: ["1 count", "2 counts", "4 counts", "8 counts"]}, {id: 1035, title: "Center cursor when resuming mouse capture", choices: ["Off", "On"]}, {id: 1036, title: "Mouse horizontal sensitivity", choices: ["0.25x", "0.5x", "0.75x", "1x", "1.25x", "1.5x", "2x", "3x", "4x"]}, {id: 1037, title: "Mouse vertical sensitivity", choices: ["0.25x", "0.5x", "0.75x", "1x", "1.25x", "1.5x", "2x", "3x", "4x"]}]}, {title: "Movement & Combat", options: [{id: 1015, title: "Gameplay timing", choices: ["30 Hz + interpolation", "Native delta (experimental)"]}, {id: 6000, title: "Sprint", choices: ["Off", "On"]}, {id: 6001, title: "Dash", choices: ["Off", "On"]}, {id: 6002, title: "Ground pound", choices: ["Off", "On"]}, {id: 6003, title: "Air dash", choices: ["Off", "On"]}, {id: 6004, title: "Sprint speed", choices: ["1x", "1.25x", "1.5x", "1.6x", "1.75x", "2x", "2.5x", "3x", "4x"]}, {id: 6005, title: "Dash speed", choices: ["6", "8", "10", "12", "16", "20", "30", "40"]}, {id: 6006, title: "Dash duration", choices: ["0.1 s", "0.15 s", "0.2 s", "0.25 s", "0.3 s", "0.4 s", "0.6 s"]}, {id: 6007, title: "Dash cooldown", choices: ["None", "0.25 s", "0.4 s", "0.65 s", "0.8 s", "1 s", "1.5 s", "2 s"]}, {id: 6008, title: "Ground-pound speed", choices: ["8", "12", "16", "20", "25", "30", "40", "50"]}, {id: 6009, title: "Keyboard sprint", choices: ["Hold", "Toggle"]}, {id: 6010, title: "Controller sprint", choices: ["Hold", "Toggle"]}, {id: 6011, title: "Native movement animations", choices: ["Off", "On"]}, {id: 6012, title: "Ground-pound damage", choices: ["Off", "On"]}, {id: 6013, title: "Ground-pound knockback", choices: ["Off", "On"]}, {id: 6014, title: "Ground-pound damage amount", choices: ["0.5", "1", "2", "3", "5", "10", "25", "50"]}, {id: 6015, title: "Ground-pound radius", choices: ["1", "1.5", "2", "3", "4", "6", "8", "12"]}, {id: 6016, title: "Dash animation", choices: ["Run / fall", "Ability clip"]}, {id: 6017, title: "Ground-pound damage type", choices: ["Spin attack", "Physical"]}]}, {title: "Controls & Camera", options: []}, {title: "Game Library", options: [{id: 7200, title: "Epic Mickey (Wii)", action: true, choices: ["Choose RVZ disc image", "Disc mounted", "Disc moved - locate RVZ", "Invalid disc - choose another", "Choose RVZ disc image", "Disc mounted", "Mounted for this session only"]}, {id: 7201, title: "Epic Mickey 2", readOnly: true, choices: ["Not mounted", "Original game files mounted"]}]}, {title: "Support & Updates", options: [{id: 7300, title: "Review bug / crash report", action: true, choices: ["Open preview"]}, {id: 7301, title: "Save report locally", action: true, choices: ["Export"]}, {id: 7302, title: "Report on GitHub", action: true, choices: ["Open issue draft"]}, {id: 7307, title: "Saved reports", action: true, choices: ["Open folder"]}, {id: 7310, title: "Update status", readOnly: true, choices: ["Not checked", "Checking", "Update available", "Downloading", "Ready to install", "No update available", "Check failed"]}, {id: 7303, title: "Check for updates", action: true, choices: ["Check now"]}, {id: 7304, title: "Download update", action: true, choices: ["Download"]}, {id: 7305, title: "Install downloaded update", action: true, choices: ["When game closes"]}, {id: 7308, title: "Cancel scheduled install", action: true, choices: ["Cancel"]}, {id: 7314, title: "Install when game closes", readOnly: true, choices: ["Off", "On"]}, {id: 7306, title: "Automatic updates", choices: ["Off", "On"]}, {id: 7309, title: "Update details", action: true, choices: ["Show"]}]}];
      this._actions = ["Jump", "Interact / co-op", "Spin attack", "Photo mode", "Recenter / lock-on", "Sketch action", "Thinner / electricity", "Paint / electricity", "Aim mode", "Guardian", "Pause / join", "Cursor mode", "Sketch up", "Sketch down", "Sketch left", "Sketch right", "Move left", "Move right", "Move forward", "Move back", "Camera left", "Camera right", "Camera up", "Camera down", "Switch character", "Sprint", "Dash", "Ground pound"];
      this._rows = []; this._list = new em2.shared.ItemList();
      this._list.orientation = em2.Constants.VERTICAL;
      var screenWidth=Number(_global.initVars.screenWidth); if(!(screenWidth>=640)) screenWidth=1280;
      var screenHeight=Number(_global.initVars.screenHeight); if(!(screenHeight>=480)) screenHeight=720;
      var offset=Number(_global.offsetX); if(isNaN(offset)) offset=0;
      var labelWidth=screenWidth*0.84;
      var i = 0;
      while(i < 8)
      {
         var row = this.attachMovie("GenericButton","pc_native_row_" + i,200+i);
         row._xscale=100; row._yscale=100; row.labelAlign="center";
         var nativeCenter=row.labelclip._x+row.t_label._x+row.t_label._width/2;
         row._x=offset+screenWidth/2-nativeCenter; row._y=screenHeight*(0.235+i*0.065);
         row.hitarea._x=nativeCenter-labelWidth/2; row.hitarea._width=labelWidth;
         row.hitarea._height=36; row.pcLabelWidth=labelWidth;
         row.pcRow = i;
         row.addEventListener(em2.Events.PRESS,mx.utils.Delegate.create(this,this._mouseSelect));
         this._rows.push(row); i++;
      }
      this._list.items = this._rows;
      this.createTextField("pc_help",400,offset+screenWidth*0.08,screenHeight*0.765,labelWidth,screenHeight*0.08);
      var format=this._rows[0].t_label.getTextFormat(); format.size=18; format.align="center";
      this.pc_help.embedFonts=true; this.pc_help.setNewTextFormat(format);
      this.pc_help.textColor = 16777215; this.pc_help.multiline = true;
      this.pc_help.wordWrap = true; this.pc_help.selectable = false;
      this._refresh();
   }
   function _label(row, label, item)
   {
      row._visible = label != undefined; if(label == undefined) row.disable(); else row.enable();
      row.pcWanted = label != undefined;
      row.pcItem = item; if(label != undefined) { row.label=label; row.t_label._xscale=100; }
   }
   function _keyName(code)
   {
      var pads = ["A","B","X","Y","LB","RB","LT","RT","LS click","RS click","Start","Back","D-pad up","D-pad down","D-pad left","D-pad right","LS left","LS right","LS up","LS down","RS left","RS right","RS up","RS down","LB + RB"];
      if(code == 0) return "Unbound";
      if(code >= 256 && code < 281) return pads[code-256];
      if(code >= 65 && code <= 90 || code >= 48 && code <= 57) return String.fromCharCode(code);
      var keys = {k1:"Mouse left",k2:"Mouse right",k4:"Mouse middle",k5:"Mouse 4",k6:"Mouse 5",k8:"Backspace",k9:"Tab",k13:"Enter",k16:"Shift",k17:"Ctrl",k18:"Alt",k27:"Esc",k32:"Space",k37:"Left",k38:"Up",k39:"Right",k40:"Down",k46:"Delete",k112:"F1",k117:"F6",k512:"Wheel up",k513:"Wheel down",k514:"Wheel left",k515:"Wheel right"};
      return keys["k"+code] == undefined ? "Key " + code : keys["k"+code];
   }
   function _options()
   {
      var demo = _level0.SetOption(7203,-1) == 1;
      if(this._category != 5) {
         var source = this._definitions[this._category].options;
         if(!demo) return source;
         var visible = []; var n=0;
         while(n<source.length) { var option=source[n++]; if(option.id!=1015 && option.id!=1021 && option.id!=1025) visible.push(option); }
         return visible;
      }
      var result = []; var i = 0;
      while(i < this._actions.length)
      {
         result.push({title:this._actions[i],id:2000+this._profile*32+i,binding:true}); i++;
      }
      var names = ["Camera X sensitivity","Camera Y sensitivity","Invert camera X","Invert camera Y","Camera X deadzone","Camera Y deadzone","Camera X curve","Camera Y curve"];
      var strengths = ["0.5x","0.6x","0.75x","0.85x","1x","1.25x","1.5x","2x"];
      var zones = ["Original","0%","5%","10%","15%","20%","25%","30%"];
      i = 0;
      while(i < 8) { result.push({title:names[i],id:2300+this._profile*8+i,choices:i<2?strengths:(i<4?["Off","On"]:(i<6?zones:["Original","Precise","Fast"]))}); i++; }
      return result;
   }
   function _refresh()
   {
      var i = 0; while(i < 8) { this._rows[i].pcWanted=false; i++; }
      if(this._view == 0)
      {
         var row=0; var demo=_level0.SetOption(7203,-1)==1;
         i = 0; while(i < this._definitions.length) { if(!demo || i!=2) { this._label(this._rows[row++],this._definitions[i].title,{category:i}); } i++; }
         this.pc_help.text = "Select a category. All changes apply immediately. Back returns to game options.";
         this._finishRows();
         return;
      }
      var options = this._options(); var pages = Math.ceil(options.length / 5);
      this._page = (this._page + pages) % pages;
      if(this._category == 5)
      {
         var profiles = ["Player 1 / Keyboard + Mouse","Player 1 / Controller","Player 2 / Keyboard + Mouse","Player 2 / Controller"];
         this._label(this._rows[0],profiles[this._profile],{profile:true});
         this._label(this._rows[1],"Apply modern control preset",{preset:true});
      }
      else
      {
         this._label(this._rows[0],this._definitions[this._category].title + " / Categories",{home:true});
         if(this._category == 6) this._label(this._rows[1],"Original files are kept unchanged",{readOnly:true});
         else if(this._category == 7) this._label(this._rows[1],"Reports stay local until you submit them",{readOnly:true});
         else this._label(this._rows[1],"Restore this category",{reset:true});
      }
      i = 0;
      while(i < 5)
      {
         var option = options[this._page*5+i];
         if(option != undefined)
         {
            var value = _level0.SetOption(option.id,-1);
            this._label(this._rows[i+2],option.title + ": " + (option.binding?this._keyName(value):option.choices[value]),option);
         }
         i++;
      }
      this._label(this._rows[7],"Page " + (this._page+1) + " / " + pages + "  -  Next",{page:true});
      this.pc_help.text = this._category == 5 ? "Select to bind. Left / Delete clears. Esc or hold Start cancels capture. Presets keep menu navigation standard." : "A / Enter: change. Left / Right: adjust. Page row: previous / next. Back: categories.";
      if(this._category == 5 && (this._profile & 1) != 0) this.pc_help.text = "Select: bind. Left/Delete: clear. Esc/hold Start: cancel. Pad preset: tap B to dash; hold B to call/interact. RB interacts immediately.";
      if(this._category == 6) this.pc_help.text = "Select Epic Mickey to locate your RVZ. Its location is remembered. EM1 level compatibility is experimental.";
      if(this._category == 3) this.pc_help.text = "Mouse options apply live to Player 1, including when playing as Oswald. Center options reset the aiming cursor. Mouse sensitivity multiplies your keyboard/mouse camera profile.";
      if(this._category == 7) this.pc_help.text = "Review and save a report before opening an issue. Verified updates install after normal exit. Downloads require a published Beetleworx release.";
      this._finishRows();
   }
   function _finishRows() { var i=0; while(i<8) { if(!this._rows[i].pcWanted) this._label(this._rows[i],undefined,undefined); i++; } }
   function _mouseSelect(event) { this._list.activeItem = event.button; this.selectActive(); }
   function _adjust(direction)
   {
      if(this._capturing) return;
      var row = this._list.activeItem; if(row == undefined) return;
      this._selected = row.pcRow; var item = row.pcItem;
      if(item.category != undefined) { this._category=item.category; this._view=1; this._page=0; }
      else if(item.home) { this._view=0; this._page=0; }
      else if(item.profile) { this._profile=(this._profile+direction+4)%4; this._page=0; }
      else if(item.preset) { _level0.SetOption(2210,this._profile); if((this._profile&1)==0) _level0.SetOption(1020,1); }
      else if(item.page) this._page += direction;
      else if(item.readOnly) return;
      else if(item.action) _level0.SetOption(item.id,1);
      else if(item.reset) { var options=this._options(); var j=0; while(j<options.length) { if(!options[j].readOnly && !options[j].action) _level0.SetOption(options[j].id,-2); j++; } }
      else if(item.binding)
      {
         if(direction<0) { _level0.SetOption(item.id,-3); this._refresh(); this._list.activate(this._selected); return; }
         _level0.SetOption(item.id,-2); this._capturing=true; this._list.deactivate(); row.label="Press an input..."; return;
      }
      else if(item.id != undefined) { var value=_level0.SetOption(item.id,-1); _level0.SetOption(item.id,(value+direction+item.choices.length)%item.choices.length); }
      this._refresh(); this._list.activate(this._selected); em2.SFX.play(em2.SFX.MM_SELECT);
   }
   function selectActive() { this._adjust(1); return true; }
   function _clear()
   {
      var item=this._list.activeItem.pcItem;
      this._selected=this._list.activeItem.pcRow;
      if(!this._capturing && item.binding) { _level0.SetOption(item.id,-3); this._refresh(); this._list.activate(this._selected); }
   }
   function onKeyDown()
   {
      if(!this._enabled || this._capturing) return;
      var key=Key.getCode();
      if(key==37) this._adjust(-1); else if(key==39) this._adjust(1); else if(key==46) this._clear();
   }
   function onEnterFrame()
   {
      if(this._enabled && this._capturing && _level0.SetOption(2200,-1)==0)
      { this._capturing=false; this._refresh(); this._list.activate(this._selected); }
      if(this._enabled && !this._capturing && ++this._refreshTick>=15) { this._refreshTick=0; this._refresh(); }
   }
   function back()
   {
      if(this._capturing) { _level0.SetOption(2000+this._profile*32,-5); this._capturing=false; this._refresh(); this._list.activate(this._selected); return true; }
      if(this._view!=0 && 1==0) { this._view=0; this._page=0; this._refresh(); this._list.activate(0); return true; }
      return false;
   }
   function enable() { this._enabled=true; _level0.SetOption(2201,1); this._refresh(); this._list.activate(0); Key.addListener(this); }
   function disable() { this._enabled=false; this._capturing=false; _level0.SetOption(2201,0); this._list.deactivate(); Key.removeListener(this); }
   function activate() { this.enable(); _level0.APressed=mx.utils.Delegate.create(this,this.selectActive); }
   function deactivate() { this.disable(); delete _level0.APressed; }
   function dispose() { this.disable(); this._list.dispose(); }
   function get threeDEnabled() { return false; }
   function set threeDEnabled(value) { this.b_threeDSettings._visible=false; }
}
