"""Generate the two actual Scaleform options classes from the shared page source."""
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[1]


def generate():
    titles = ['Display & Performance', 'Rendering & Lighting', 'HDR Calibration', 'Camera', 'Movement & Combat', 'Controls & Camera', 'Game Library', 'Support & Updates']
    pages = [dict(title=title, options=[]) for title in titles]
    pages[7]['options'] = [
        dict(id=7300,title='Review bug / crash report',action=True,choices=['Open preview']),
        dict(id=7301,title='Save report locally',action=True,choices=['Export']),
        dict(id=7302,title='Report on GitHub',action=True,choices=['Open issue draft']),
        dict(id=7307,title='Saved reports',action=True,choices=['Open folder']),
        dict(id=7310,title='Update status',readOnly=True,choices=['Not checked','Checking','Update available','Downloading','Ready to install','No update available','Check failed']),
        dict(id=7303,title='Check for updates',action=True,choices=['Check now']),
        dict(id=7304,title='Download update',action=True,choices=['Download']),
        dict(id=7305,title='Install downloaded update',action=True,choices=['When game closes']),
        dict(id=7308,title='Cancel scheduled install',action=True,choices=['Cancel']),
        dict(id=7314,title='Install when game closes',readOnly=True,choices=['Off','On']),
        dict(id=7306,title='Automatic updates',choices=['Off','On']),
        dict(id=7309,title='Update details',action=True,choices=['Show'])]
    pages[6]['options'] = [
        dict(id=7200, title='Epic Mickey (Wii)', action=True,
             choices=['Choose RVZ disc image', 'Disc mounted', 'Disc moved - locate RVZ', 'Invalid disc - choose another', 'Choose RVZ disc image', 'Disc mounted', 'Mounted for this session only']),
        dict(id=7201, title='Epic Mickey 2', readOnly=True, choices=['Not mounted', 'Original game files mounted'])]
    source = (ROOT / 'runtime/engine_settings_catalog.h').read_text()
    for index, page, title, choices in re.findall(r'\{(\d+),(\d+),"([^"]+)","([^"]+)"\}', source):
        pages[int(page)]['options'].append(dict(id=1000+int(index), title=title, choices=choices.split('|')))
    pages[1]['options'].append(dict(id=1989,title='Original Mickey ink assets',readOnly=True,
        choices=['Not loaded - requires mounted EM1 assets', 'Loaded']))
    mods = [('Sprint', ['Off','On']), ('Dash', ['Off','On']), ('Ground pound', ['Off','On']),
            ('Air dash', ['Off','On']), ('Sprint speed', ['1x','1.25x','1.5x','1.6x','1.75x','2x','2.5x','3x','4x']),
            ('Dash speed', ['6','8','10','12','16','20','30','40']),
            ('Dash duration', ['0.1 s','0.15 s','0.2 s','0.25 s','0.3 s','0.4 s','0.6 s']),
            ('Dash cooldown', ['None','0.25 s','0.4 s','0.65 s','0.8 s','1 s','1.5 s','2 s']),
            ('Ground-pound speed', ['8','12','16','20','25','30','40','50']),
            ('Keyboard sprint', ['Hold','Toggle']), ('Controller sprint', ['Hold','Toggle']),
            ('Native movement animations', ['Off','On']), ('Ground-pound damage', ['Off','On']),
            ('Ground-pound knockback', ['Off','On']), ('Ground-pound damage amount', ['0.5','1','2','3','5','10','25','50']),
            ('Ground-pound radius', ['1','1.5','2','3','4','6','8','12']), ('Dash animation', ['Run / fall','Ability clip']),
            ('Ground-pound damage type', ['Spin attack','Physical'])]
    for index, (title, choices) in enumerate(mods):
        pages[4]['options'].append(dict(id=6000+index,title=title,choices=choices))
    actions = ['Jump','Interact / co-op','Spin attack','Photo mode','Recenter / lock-on','Sketch action','Thinner / electricity','Paint / electricity','Aim mode','Guardian','Pause / join','Cursor mode','Sketch up','Sketch down','Sketch left','Sketch right','Move left','Move right','Move forward','Move back','Camera left','Camera right','Camera up','Camera down','Switch character','Sprint','Dash','Ground pound']
    template = (ROOT / 'assets/pc-menu/NativeOptions.as.in').read_text()
    for name, category, view in [('TVSettings',0,0),('PlayerControls',5,1)]:
        text = template.replace('CLASS_NAME',name).replace('INITIAL_CATEGORY',str(category)).replace('INITIAL_VIEW',str(view))
        definitions = re.sub(r'"([A-Za-z_][A-Za-z0-9_]*)":',r'\1:',json.dumps(pages))
        text = text.replace('MENU_DEFINITIONS',definitions).replace('ACTION_DEFINITIONS',json.dumps(actions))
        path = ROOT / ('assets/pc-menu/'+name+'.as')
        if not path.exists() or path.read_text()!=text:
            path.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    generate()
