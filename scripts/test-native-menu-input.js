// Execute the shared ActionScript method bodies with native callback fixtures.
// The real GFx compiler round trip is checked separately by the menu builders.
const fs = require('fs');
const vm = require('vm');
const assert = require('assert/strict');
const source = fs.readFileSync(require('path').join(__dirname, '../assets/pc-menu/NativeMenuInput.as'), 'utf8');
let clock = 1, selected = 0, rollovers = 0, backs = 0, changed = 0;
let pointer = {2220:32767,2221:32767,2223:1,2224:1,2225:0,2226:32767,2227:32767,2228:32767,2229:32767};
const level = { SetOption: id => pointer[id], APressed: () => ++selected };
const context = vm.createContext({ Array, Math, Number, isNaN, getTimer: () => clock,
  _level0: level, _global: { initVars: { screenWidth:1280,screenHeight:720 }, offsetX:0 },
  em2: { Events: { SLIDER_CHANGED:'slider' } } });
vm.runInContext(source+'\nthis.methods={_pcPrompt:_pcPrompt,_pcMenuInput:_pcMenuInput};', context);
const bounds = {xMin:500,xMax:800,yMin:300,yMax:420};
const button = { _visible:true,isEnabled:true,hitarea:{getBounds:()=>bounds},_rollover:()=>++rollovers };
const owner = { ...context.methods,_visible:true,_list:{_active:true,_items:[button]},tvsettings:{},playerControls:{} };
const tick = () => { ++clock; owner._pcMenuInput(); };
tick(); assert.equal(rollovers,0,'opening a page does not treat a parked cursor as new mouse input');
pointer[2223]++; tick(); assert.equal(rollovers,1);
tick(); assert.equal(rollovers,1,'parked mouse must not steal controller selection');
pointer[2225]++; tick(); assert.equal(selected,1);
tick(); assert.equal(selected,1,'one mouse release activates once');
pointer[2225]++; pointer[2228]=0; tick(); assert.equal(selected,1,'pressing outside then releasing inside is not a click');
pointer[2228]=32767; pointer[2225]++; owner._parent={_visible:false}; tick(); assert.equal(selected,1,'hidden ancestor blocks interaction');
owner._parent=undefined; owner._list._active=false; pointer[2225]++; tick(); assert.equal(selected,1,'inactive pages cannot receive clicks');
owner._list._active=true; button.pcRow=0; owner.selectActive=()=>++changed; pointer[2225]++; tick();
assert.equal(changed,1); assert.equal(selected,1,'custom setting uses its own action instead of confirming twice');
delete button.pcRow; button._sliderWidth=100; button.max=100; button.step=5; button.dispatchEvent=()=>++changed;
pointer[2225]++; tick(); assert.equal(button.position,45); assert.equal(changed,2);
delete button._sliderWidth;
const back={...button,isEnabled:false}; owner.b_back=back; owner._goBack=()=>++backs; owner._list._items=[];
pointer[2225]++; tick(); assert.equal(backs,1,'native footer remains mouse-accessible while disabled for pad dispatch');
console.log('Native menu input passed: hover, controller ownership, single activation, press/release bounds, visibility, active page, setting actions, sliders and Back.');
