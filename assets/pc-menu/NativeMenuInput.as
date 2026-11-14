   function _pcPrompt(button,back,style)
   {
      if(button==undefined || button.icon==undefined) return;
      if(button.pcPrompt==undefined) {
         button.createTextField("pcPrompt",900,button.icon._x-30,button.icon._y,68,30);
         var format=button.t_label.getNewTextFormat(); format.size=16; format.align="center";
         button.pcPrompt.embedFonts=false; button.pcPrompt.setNewTextFormat(format);
         button.pcPrompt.selectable=false; button.pcPrompt.textColor=16777215;
      }
      button.icon._visible=style==2;
      button.pcPrompt._visible=style==1 || style==4;
      button.pcPrompt.text=style==1?(back?"Esc":"Enter"):style==3?(back?"Circle":"Cross"):(back?"A":"B");
      if(button.pcGlyph==undefined) button.createEmptyMovieClip("pcGlyph",899);
      var glyph=button.pcGlyph; glyph._visible=style!=2;
      if(button.pcPromptStyle==style) return;
      button.pcPromptStyle=style; glyph.clear();
      glyph._x=button.icon._x; glyph._y=button.icon._y+4;
      glyph.lineStyle(2,style==3?(back?15618642:5213695):13421772,100);
      if(style==1) {
         glyph.moveTo(-28,0); glyph.lineTo(36,0); glyph.lineTo(36,25); glyph.lineTo(-28,25); glyph.lineTo(-28,0);
      } else if(style==3 && !back) {
         glyph.moveTo(0,2); glyph.lineTo(20,22); glyph.moveTo(20,2); glyph.lineTo(0,22);
      } else {
         var i=0; glyph.moveTo(22,12);
         while(i<=16) { var angle=i++*Math.PI/8; glyph.lineTo(10+Math.cos(angle)*12,12+Math.sin(angle)*12); }
      }
   }
   function _pcMenuInput()
   {
      if(!this._visible || _level0.SetOption==undefined) return;
      var ancestor=this._parent;
      while(ancestor!=undefined) { if(ancestor._visible==false) return; ancestor=ancestor._parent; }
      var style=_level0.SetOption(2224,-1);
      this._pcPrompt(this.b_select,false,style); this._pcPrompt(this.b_back,true,style);
      var list=this._list;
      if(list!=undefined && !list._active && (this.tvsettings._enabled || this.playerControls._enabled || this._showingSavingMessage)) list={_active:true,_items:[]};
      if(list==undefined || !list._active) return;
      var now=getTimer();
      if(_level0._pcPointerFrame!=now) {
         _level0._pcPointerFrame=now;
         _level0._pcPointerX=_level0.SetOption(2220,-1);
         _level0._pcPointerY=_level0.SetOption(2221,-1);
         _level0._pcPointerMove=_level0.SetOption(2223,-1);
         _level0._pcPointerClick=_level0.SetOption(2225,-1);
         _level0._pcClickX=_level0.SetOption(2226,-1); _level0._pcClickY=_level0.SetOption(2227,-1);
         _level0._pcDownX=_level0.SetOption(2228,-1); _level0._pcDownY=_level0.SetOption(2229,-1);
      }
      if(this._pcLastClick==undefined) this._pcLastClick=_level0._pcPointerClick;
      if(this._pcLastMove==undefined) this._pcLastMove=_level0._pcPointerMove;
      var clicked=this._pcLastClick!=_level0._pcPointerClick;
      var moved=this._pcLastMove!=_level0._pcPointerMove;
      this._pcLastClick=_level0._pcPointerClick; this._pcLastMove=_level0._pcPointerMove;
      if(!moved && !clicked) return;
      var width=Number(_global.initVars.screenWidth); if(!(width>=640)) width=1280;
      var height=Number(_global.initVars.screenHeight); if(!(height>=480)) height=720;
      var offset=Number(_global.offsetX); if(isNaN(offset)) offset=0;
      var x=offset+_level0._pcPointerX*width/65535;
      var y=_level0._pcPointerY*height/65535;
      var cx=offset+_level0._pcClickX*width/65535; var cy=_level0._pcClickY*height/65535;
      var dx=offset+_level0._pcDownX*width/65535; var dy=_level0._pcDownY*height/65535;
      var items=[]; var i=0; var j=0;
      while(i<list._items.length) {
         var entry=list._items[i++];
         if(entry instanceof Array) { j=0; while(j<entry.length) { items.push(entry[j++]); } }
         else items.push(entry);
      }
      if(this.b_back!=undefined) items.push(this.b_back);
      if(this.b_select!=undefined) items.push(this.b_select);
      if(this._pcMouseItems!=undefined) items=items.concat(this._pcMouseItems);
      var hovered=undefined;
      i=0;
      while(i<items.length) {
         var item=items[i++];
         if(item==undefined || !item._visible || (item.isEnabled==false && item!=this.b_back && item!=this.b_select)) continue;
         var bounds=item.hitarea!=undefined?item.hitarea.getBounds(_level0):item.getBounds(_level0);
         var hover=_level0._pcPointerX>=0 && _level0._pcPointerY>=0 && x>=bounds.xMin && x<=bounds.xMax && y>=bounds.yMin && y<=bounds.yMax;
         if(hover && hovered==undefined) hovered=item;
         if(hover && moved && item!=this.b_back && item._rollover!=undefined && this._pcHovered!=item) {
            if(this._pcHovered!=undefined && this._pcHovered._rollout!=undefined) this._pcHovered._rollout();
            this._pcHovered=item; item._rollover();
         }
         if(clicked && _level0._pcConsumedClick!=_level0._pcPointerClick &&
            _level0._pcClickX>=0 && _level0._pcClickY>=0 && _level0._pcDownX>=0 && _level0._pcDownY>=0 &&
            cx>=bounds.xMin && cx<=bounds.xMax && cy>=bounds.yMin && cy<=bounds.yMax &&
            dx>=bounds.xMin && dx<=bounds.xMax && dy>=bounds.yMin && dy<=bounds.yMax) {
            _level0._pcConsumedClick=_level0._pcPointerClick;
            if(item._pcClick!=undefined) item._pcClick();
            else if(item==this.b_back) { if(this._goBack!=undefined) this._goBack(); else item._press(); }
            else if(item==this.b_select) { if(_level0.APressed!=undefined) _level0.APressed(); }
            else if(item._sliderWidth!=undefined) {
               item.position=Math.round((cx-bounds.xMin)/(bounds.xMax-bounds.xMin)*item.max/item.step)*item.step;
               item.dispatchEvent({type:em2.Events.SLIDER_CHANGED,slider:item});
            }
            else if(item.pcRow!=undefined) { list.activeItem=item; this.selectActive(); }
            else { item._rollover(); if(_level0.APressed!=undefined) _level0.APressed(); }
            return;
         }
      }
      if(moved && hovered==undefined && this._pcHovered!=undefined) {
         if(this._pcHovered._rollout!=undefined) this._pcHovered._rollout();
         this._pcHovered=undefined;
      }
   }
   function _pcMenuTree(node,depth)
   {
      if(node==undefined || !node._visible || depth>5) return;
      if(node._list!=undefined) {
         if(node._pcMenuInput==undefined) { node._pcMenuInput=this._pcMenuInput; node._pcPrompt=this._pcPrompt; }
         node._pcMenuInput();
      }
      for(var name in node) {
         var child=node[name];
         if(child instanceof MovieClip && child._parent==node) this._pcMenuTree(child,depth+1);
      }
   }
