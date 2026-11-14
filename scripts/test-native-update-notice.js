const fs=require('fs'),vm=require('vm'),assert=require('assert/strict');
const source=fs.readFileSync(require('path').join(__dirname,'../assets/pc-menu/NativeUpdateNotice.as'),'utf8');
let pending=0,state=0,calls=[],shown=[];
const dialog={isActive:false,b_confirm:{},b_cancel:{},t_title:{text:''},
  summon(data){this.isActive=true;this.data=data;this.t_title.text=data.title;shown.push(data);},
  hide(){this.isActive=false;}};
const level={SetOption(id,choice){calls.push([id,choice]);if(id===7315)return pending;if(id===7310)return state;
  if(id===7316){pending=0;if(choice===1)state=3;}if(id===7318)assert.equal(state,4,'restart requires verified Ready state');return 1;}};
const context=vm.createContext({_level0:level,mx:{utils:{Delegate:{create:(owner,fn)=>fn.bind(owner)}}}});
vm.runInContext(source+'\nthis.methods={_pcUpdateDialog,_pcUpdateAccept,_pcUpdateLater,_pcUpdateKeepPlaying,_pcUpdateTick};',context);
const owner=()=>({...context.methods,_visible:true,_parent:{dialog},_pcMenuInput(){},_pcPrompt(){},
  _em2OnlineRead(id){return id===7313?'0.1.0-demo.7':'Download failed';}});
let page=owner();page._pcUpdateTick();assert.equal(shown.length,0);
pending=1;state=2;page._pcUpdateTick();assert.equal(shown.length,1);
assert.equal(dialog.data.confirm,'Update');assert.equal(dialog.data.cancel,'Not now');
assert.equal(dialog.b_select,dialog.b_confirm,'uses the original Flash dialog controls');
dialog.data.callbackWhenCancelling();dialog.hide();page._pcUpdateTick();assert.equal(shown.length,1);
assert(calls.some(c=>c[0]===7316&&c[1]===0),'Not now reaches the native dismissal action');
page=owner();pending=1;page._pcUpdateTick();let accept=dialog.data.callback;dialog.hide();accept();page._pcUpdateTick();
assert.equal(dialog.data.title,'Updating Beetleworx');assert.equal(dialog.data.cancel,'Keep playing');
state=4;page._pcUpdateTick();page._pcUpdateTick();assert.equal(calls.filter(c=>c[0]===7318).length,1,'one verified update requests one restart');
page=owner();pending=1;state=2;page._pcUpdateTick();accept=dialog.data.callback;dialog.hide();accept();page._pcUpdateTick();
dialog.data.callbackWhenCancelling();dialog.hide();state=4;page._pcUpdateTick();assert.equal(calls.filter(c=>c[0]===7318).length,1,'Keep playing prevents an automatic restart');
page=owner();pending=1;state=2;page._pcUpdateTick();accept=dialog.data.callback;dialog.hide();accept();page._pcUpdateTick();
state=6;page._pcUpdateTick();assert.equal(dialog.data.title,'Update could not finish');assert.equal(dialog.data.confirm,'OK');dialog.hide();
page=owner();pending=1;state=2;dialog.isActive=true;dialog.t_title.text='Existing game notification';
const before=shown.length;page._pcUpdateTick();assert.equal(shown.length,before,'existing notifications are not replaced');
dialog.hide();page._pcUpdateTick();assert.equal(shown.length,before+1,'offer waits for the original notification to close');
console.log('Native update notice passed: original Flash dialog, Update/Not now, dismissal, progress, verified-only single restart, Keep playing, failure and notification ownership.');
