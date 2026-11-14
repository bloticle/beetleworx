   function _em2Field(parent,name,x,y,width,height,size)
   {
      parent.createTextField(name,parent.getNextHighestDepth(),x,y,width,height);
      var field=parent[name]; var format=this.b_main.t_label.getTextFormat();
      format.size=size; format.align="left"; field.embedFonts=true;
      field.setNewTextFormat(format); field.selectable=false; field.textColor=16777215;
      field.multiline=true; field.wordWrap=true; return field;
   }
   function _em2Rect(clip,width,height,color)
   {
      clip.clear(); clip.lineStyle(1,6710886,100); clip.beginFill(color,94);
      clip.moveTo(0,0); clip.lineTo(width,0); clip.lineTo(width,height);
      clip.lineTo(0,height); clip.lineTo(0,0); clip.endFill();
   }
   function _em2ActionRect(index)
   {
      if(this._em2LobbyMode) return {x:0.06+(index%3)*0.30,y:0.76+Math.floor(index/3)*0.075,width:0.28};
      if(index==5 || (this._em2JoinPage && index==2)) return {x:0.31,y:this._em2JoinPage?0.54:0.75,width:0.38};
      return {x:0.10+(index%2)*0.42,y:0.30+Math.floor(index/2)*0.15,width:0.38};
   }
   function _em2MoveSelection(dx,dy)
   {
      if(!this._list._active) return;
      var active=this._list.activeItem;
      if(active==undefined) { this._list.activate(0); return; }
      var origin=active.hitarea.getBounds(this); var ox=(origin.xMin+origin.xMax)/2; var oy=(origin.yMin+origin.yMax)/2;
      var best=undefined; var score=10000000; var i=0;
      while(i<this._items.length) {
         var item=this._items[i++];
         if(item==active || !item._visible || !item.isEnabled) continue;
         var bounds=item.hitarea.getBounds(this);
         var x=(bounds.xMin+bounds.xMax)/2-ox; var y=(bounds.yMin+bounds.yMax)/2-oy;
         var along=x*dx+y*dy; var across=Math.abs(x*dy-y*dx);
         if(along>1 && along+across*4<score) { score=along+across*4; best=item; }
      }
      if(best!=undefined) this._list.activeItem=best;
   }
   function _em2OnlineLayout(state)
   {
      var width=Number(_global.initVars.screenWidth); if(!(width>=640)) width=1280;
      var height=Number(_global.initVars.screenHeight); if(!(height>=480)) height=720;
      var offset=Number(_global.offsetX); if(isNaN(offset)) offset=0;
      this._em2LobbyMode=(state==2 || state==6) && _level0.SetOption(2731,-1)==1 && !this._em2OnlinePlayerPage && !this._em2OnlineRosterPage && this._em2OnlineVoicePage==0 && !this._em2KickConfirm;
      this._pcMouseItems=[];
      if(this._em2OnlineHeading==undefined) this._em2Field(this,"_em2OnlineHeading",offset+width*0.06,height*0.09,width*0.88,44,28);
      this._em2OnlineHeading._visible=true;
      this._em2OnlineHeading.text=this._em2KickConfirm?"Remove player?":this._em2LobbyMode?"Lobby":this._em2OnlineVoicePage>0?"Voice chat":this._em2OnlinePlayerPage?"Your player":this._em2OnlineRosterPage?"Manage players":"Online Multiplayer";
      if(this._em2CopyCode!=undefined) this._em2CopyCode._visible=this._em2LobbyMode;
      if(this._em2LobbyMode) {
         this._em2OnlineHeading.text="Lobby   "+this._em2OnlineRead(2711);
         if(this._em2CopyCode==undefined) {
            var copy=this.createEmptyMovieClip("_em2CopyCode",this.getNextHighestDepth()); copy._x=offset+width*0.88; copy._y=height*0.10;
            this._em2Rect(copy,34,32,3422782); copy.lineStyle(2,16777215,100);
            copy.moveTo(8,7); copy.lineTo(23,7); copy.lineTo(23,23); copy.lineTo(8,23); copy.lineTo(8,7);
            copy.moveTo(12,3); copy.lineTo(27,3); copy.lineTo(27,19);
            copy.hitarea=copy; copy.isEnabled=true; copy._rollover=function() { this._parent._em2OnlineHelp.text="Copy room code and send it to a friend."; };
            copy._pcClick=function() { _level0.SetOption(2700,5); this._parent._em2OnlineHelp.text="Room code copied."; };
         }
         this._pcMouseItems.push(this._em2CopyCode);
      }
      this._em2OnlineHelp._x=offset+width*0.06; this._em2OnlineHelp._y=height*0.16;
      this._em2OnlineHelp._width=width*0.88; this._em2OnlineHelp._height=height*0.09;
      if(this._em2LobbyPanel!=undefined) this._em2LobbyPanel._visible=this._em2LobbyMode;
      if(this._em2LobbyMode) this._em2RefreshLobbyPanels(width,height,offset);
   }
   function _em2Silhouette(clip,x,y,rx,ry,color)
   {
      clip.beginFill(color,100); var i=0;
      clip.moveTo(x+rx,y);
      while(i<=16) { var a=i++*Math.PI/8; clip.lineTo(x+Math.cos(a)*rx,y+Math.sin(a)*ry); }
      clip.endFill();
   }
   function _em2KickPlayer(index)
   {
      if(_level0.SetOption(2775,index)!=1) return;
      _level0.SetOption(2759,2);
      this._em2KickConfirm=true; this._em2OnlineRefresh(); this._list.activate(1);
   }
   function _em2RefreshLobbyPanels(width,height,offset)
   {
      if(this._em2LobbyPanel==undefined) {
         var panel=this.createEmptyMovieClip("_em2LobbyPanel",this.getNextHighestDepth());
         panel._x=offset+width*0.06; panel._y=height*0.27;
         this._em2Rect(panel,width*0.88,height*0.43,1579544);
         this._em2Field(panel,"playersTitle",16,10,width*0.40,28,20).text="Players";
         this._em2Field(panel,"chatTitle",width*0.46,10,width*0.40,28,20).text="Chat";
         var i=0;
         while(i<16) {
            var row=panel.createEmptyMovieClip("player"+i,panel.getNextHighestDepth());
            row._x=16+(i%2)*width*0.215; row._y=48+Math.floor(i/2)*height*0.040;
            row.hitarea=row.createEmptyMovieClip("area",1); this._em2Rect(row.hitarea,width*0.205,height*0.038,2106152);
            row.portrait=row.createEmptyMovieClip("portrait",2);
            this._em2Field(row,"name",30,3,width*0.16,height*0.035,15);
            row.index=i; row.isEnabled=true; row._rollover=function() {};
            row._pcClick=function() { this._parent._parent._em2KickPlayer(this.index); };
            i++;
         }
         i=0; while(i<6) { this._em2Field(panel,"message"+i,width*0.46,44+i*height*0.047,width*0.40,height*0.045,14); i++; }
         var dock=panel.createEmptyMovieClip("chatDock",panel.getNextHighestDepth());
         dock._x=width*0.46; dock._y=height*0.36; dock.hitarea=dock;
         this._em2Rect(dock,width*0.40,height*0.055,3422782); dock.isEnabled=true; dock._rollover=function() {};
         dock._pcClick=function() { _level0.SetOption(2759,0); };
         this._em2Field(dock,"draft",8,4,width*0.38,height*0.05,16);
         this._em2Field(panel,"keyboard",width*0.46,48,width*0.40,height*0.27,18);
      }
      var panel=this._em2LobbyPanel; panel._visible=true;
      var count=_level0.SetOption(2770,-1); panel.playersTitle.text="Players ("+count+"/"+_level0.SetOption(2736,-1)+")";
      var i=0;
      while(i<16) {
         var row=panel["player"+i]; row._visible=i<count;
         if(i<count) {
            var name=this._em2OnlineRead(2771,i*128); var rgb=_level0.SetOption(2773,i); var flags=_level0.SetOption(2774,i);
            row.name.text=name; row.name.textColor=rgb;
            row.portrait.clear();
            var oswald=_level0.SetOption(2772,i)==1;
            this._em2Silhouette(row.portrait,13,15,6,6,rgb);
            this._em2Silhouette(row.portrait,oswald?9:6,oswald?5:8,oswald?3:4,oswald?7:4,rgb);
            this._em2Silhouette(row.portrait,oswald?17:20,oswald?5:8,oswald?3:4,oswald?7:4,rgb);
            if((flags&4)!=0) { row.portrait.lineStyle(2,5635925,100); row.portrait.moveTo(22,19); row.portrait.lineTo(25,22); row.portrait.lineTo(30,15); }
            if((flags&8)!=0) {
               var right=width*0.205-12; row.portrait.lineStyle(2,15618642,100);
               row.portrait.moveTo(right-8,8); row.portrait.lineTo(right,16);
               row.portrait.moveTo(right,8); row.portrait.lineTo(right-8,16);
            }
            row.isEnabled=(flags&8)!=0; this._pcMouseItems.push(row);
         }
         i++;
      }
      var chat=_level0.SetOption(2750,-1); var editing=(chat&2)!=0;
      var controller=_level0.SetOption(2230,-1)==1;
      _level0.SetOption(2751,-1); var messages=_level0.SetOption(2752,-1);
      i=0; while(i<6) {
         var field=panel["message"+i]; var at=messages-6+i;
         field._visible=at>=0 && !(editing && controller);
         if(at>=0) { _level0.SetOption(2753,at); field.text=this._em2OnlineRead(2754)+": "+this._em2OnlineRead(2755); field.textColor=_level0.SetOption(2756,-1); }
         i++;
      }
      panel.chatDock.draft.text=editing?this._em2OnlineRead(2757):"Click here or press T to chat";
      panel.keyboard._visible=editing && controller;
      if(panel.keyboard._visible) panel.keyboard.text=this._em2OnlineRead(2762)+"\nA: type   X: delete   Y: send   B: close";
      this._pcMouseItems.push(panel.chatDock);
      var error=this._em2OnlineRead(2760); if(error!="") panel.chatDock.draft.text=error;
   }
   function _em2RestoreLayout()
   {
      this._pcMouseItems=[];
      if(this._em2LobbyPanel!=undefined) this._em2LobbyPanel._visible=false;
      if(this._em2OnlineHeading!=undefined) this._em2OnlineHeading._visible=false;
      if(this._em2CopyCode!=undefined) this._em2CopyCode._visible=false;
      if(this._em2JoinCode!=undefined) this._em2JoinCode._visible=false;
      var i=0; while(i<this._items.length) {
         var button=this._items[i++]; var saved=button._em2SavedLayout;
         if(saved!=undefined) {
            button._y=saved.y; button.t_label._x=saved.x; button.t_label._width=saved.width;
            button.t_label.autoSize=saved.autoSize; button.t_label.setTextFormat(saved.format);
            button.hitarea._x=saved.hitX; button.hitarea._width=saved.hitWidth;
            button.hitarea._y=saved.hitY; button.hitarea._height=saved.hitHeight;
            if(button.pcCard!=undefined) button.pcCard._visible=false;
         }
      }
   }
