#!/usr/bin/env python3
"""Settings browser data generator (run with python -I from anywhere; reads only files of this repository).

Reads the RPCS3 config tree (rpcs3/Emu/system_config.h, the PS4 + LLVM + Vulkan branch), the enum spellings
(system_config_types.cpp, cellSysutil.cpp, KeyboardHandler.cpp), the PC settings dialog (settings_dialog.ui / .cpp,
emu_settings_type.cpp, emu_settings.cpp GetLocalizedSetting, tooltips.h) and the in-game home menu pages.

Writes (generated, checked in, never edited by hand):
  ps4-port/tests/cfg-mocks/cfg_tree.inc        CFG_LEAF(key, type, min, max, default, dynamic): every leaf of cfg_root
  ps4-port/tests/cfg-mocks/cfg_enum_names.inc  CFG_MOCK_ENUM(T, {T::x, "spelling"}, ...): the YAML spellings per enum
  ps4-port/tests/cfg-mocks/cfg_pc_text.inc     CFG_PC_LABEL / CFG_PC_VALUE / CFG_PC_TIP: the PC dialog's English texts,
                                               CFG_HOME_MENU(key): settings the in-game home menu edits

Options:
  --skeleton <path>  a catalog skeleton (one CFG_ITEM row per leaf with the PC tab and label); the checked-in
                     rpcs3/util/ps4_cfg_catalog.inc is edited by hand and never overwritten
  --strings <path>   an English-only I18N skeleton of the label, value and tooltip ids
  --check            regenerate in memory and compare with the checked-in files and the catalog keys; exit 1 on a
                     difference (an upstream merge added, removed or renamed a setting or a PC text)
  --json <path>      everything parsed, for inspection
"""
import json
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
MOCKS = os.path.join(REPO, 'ps4-port', 'tests', 'cfg-mocks')
CATALOG = os.path.join(REPO, 'rpcs3', 'util', 'ps4_cfg_catalog.inc')

# The branch of every #if the PS4 build compiles (ps4-port/CMakeLists.txt: RPCS3_PS4, __ORBIS__, LLVM, Vulkan)
DEFINED = {'RPCS3_PS4', '__ORBIS__', 'LLVM_AVAILABLE', 'HAVE_VULKAN'}


def read(rel):
    with open(os.path.join(REPO, rel), encoding='utf-8') as f:
        return f.read()


# ---------------------------------------------------------------- preprocessor branches

def eval_condition(expr):
    expr = expr.strip()
    expr = re.sub(r'defined\s*\(\s*(\w+)\s*\)', lambda m: ' True ' if m.group(1) in DEFINED else ' False ', expr)
    expr = re.sub(r'defined\s+(\w+)', lambda m: ' True ' if m.group(1) in DEFINED else ' False ', expr)
    expr = expr.replace('&&', ' and ').replace('||', ' or ')
    expr = re.sub(r'!(?!=)', ' not ', expr)
    expr = re.sub(r'\b([A-Za-z_]\w*)\b', lambda m: m.group(1) if m.group(1) in ('True', 'False', 'and', 'or', 'not') else
                  (' True ' if m.group(1) in DEFINED else ' False '), expr)
    return bool(eval(expr, {'__builtins__': {}}, {}))


def active_lines(text):
    """Lines of 'text' outside inactive #if branches (the directives themselves are dropped)."""
    out = []
    stack = []  # (taking, any_taken)
    for line in text.split('\n'):
        s = line.strip()
        m = re.match(r'#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)', s)
        if m:
            kind, rest = m.group(1), m.group(2).split('//')[0].strip()
            parent = all(t for t, _ in stack)
            if kind == 'ifdef':
                c = rest.split()[0] in DEFINED
                stack.append((c, c))
            elif kind == 'ifndef':
                c = rest.split()[0] not in DEFINED
                stack.append((c, c))
            elif kind == 'if':
                c = eval_condition(rest)
                stack.append((c, c))
            elif kind == 'elif':
                taking, any_taken = stack.pop()
                c = not any_taken and eval_condition(rest)
                stack.append((c, any_taken or c))
            elif kind == 'else':
                taking, any_taken = stack.pop()
                stack.append((not any_taken, True))
            else:
                stack.pop()
            continue
        if all(t for t, _ in stack):
            out.append(line)
    return out


# ---------------------------------------------------------------- C++ expressions

def split_args(text):
    """Top-level comma split (parentheses, braces, brackets, angle brackets of casts, string literals)."""
    parts, depth, angle, cur, quote = [], 0, 0, '', None
    i = 0
    while i < len(text):
        c = text[i]
        if quote:
            cur += c
            if c == '\\' and i + 1 < len(text):
                cur += text[i + 1]
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in '"\'' and not (c == "'" and cur and cur[-1].isdigit()):
            quote = c
            cur += c
        elif c in '({[':
            depth += 1
            cur += c
        elif c in ')}]':
            depth -= 1
            cur += c
        elif c == '<' and cur and (cur[-1].isalnum() or cur[-1] == '_') and text[i + 1:i + 2] not in ('<', '='):
            angle += 1  # template argument list (static_cast<u32>), not a shift
            cur += c
        elif c == '>' and angle:
            angle -= 1
            cur += c
        elif c == ',' and depth == 0 and angle == 0:
            parts.append(cur.strip())
            cur = ''
        else:
            cur += c
        i += 1
    if cur.strip():
        parts.append(cur.strip())
    return parts


def eval_int(expr):
    e = expr.strip()
    e = re.sub(r"(?<=\d)'(?=\d)", '', e)
    e = re.sub(r'(?<=[0-9a-fA-F])(?:ULL|ull|LL|ll|UL|ul|U|u|L|l)\b', '', e)
    e = re.sub(r's32\{smin\}', str(-2 ** 31), e)
    e = re.sub(r's32\{smax\}', str(2 ** 31 - 1), e)
    e = re.sub(r'u32\{umax\}', str(2 ** 32 - 1), e)
    e = re.sub(r'u64\{umax\}', str(2 ** 64 - 1), e)
    e = re.sub(r's64\{smin\}', str(-2 ** 63), e)
    e = re.sub(r's64\{smax\}', str(2 ** 63 - 1), e)
    return int(eval(e, {'__builtins__': {}}, {}))


def c_string(literal):
    s = literal.strip()
    assert s.startswith('"') and s.endswith('"'), literal
    return bytes(s[1:-1], 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


# ---------------------------------------------------------------- enums and their spellings

def parse_enums(text):
    """enum [class] Name [: type] { a, b = 3, ... }; -> {Name: [(enumerator, value)]} (active branch only)."""
    out = {}
    lines = active_lines(text)
    body = '\n'.join(lines)
    body = re.sub(r'//[^\n]*', '', body)
    for m in re.finditer(r'enum\s+(?:class\s+)?(\w+)\s*(?::\s*[\w:]+)?\s*\{([^}]*)\}', body):
        name, items = m.group(1), m.group(2)
        value = -1
        values = []
        for item in items.split(','):
            item = item.strip()
            if not item:
                continue
            if '=' in item:
                k, v = item.split('=', 1)
                k = k.strip()
                v = v.strip()
                known = dict(values)
                v = re.sub(r'\b(\w+)\b', lambda mm: str(known[mm.group(1)]) if mm.group(1) in known else mm.group(1), v)
                try:
                    value = eval_int(v)
                except Exception:
                    continue
            else:
                k = item
                value += 1
            values.append((k, value))
        out[name] = values
    return out


def parse_spellings(text):
    """fmt_class_string<T>::format bodies -> {T: {enumerator: spelling}} (active branch only)."""
    out = {}
    body = '\n'.join(active_lines(text))
    for m in re.finditer(r'void\s+fmt_class_string<([\w:]+)>::format\s*\(.*?\)\s*\{(.*?)\n\}', body, re.S):
        t, block = m.group(1), m.group(2)
        cases = {}
        for c in re.finditer(r'case\s+([\w:]+)\s*:\s*return\s*("(?:[^"\\]|\\.)*")\s*;', block):
            cases[c.group(1).split('::')[-1]] = c_string(c.group(2))
        if cases:
            out[t] = cases
    return out


# ---------------------------------------------------------------- the config tree

LEAF_RE = re.compile(r'^\s*(cfg::[\w:]+(?:\s*<[^;{]*?>)?|fifo_setting)\s+(\w+)\s*\{\s*this\s*,\s*("(?:[^"\\]|\\.)*")\s*(.*?)\}\s*;')
STRUCT_RE = re.compile(r'^\s*struct\s+(\w+)\s*:\s*(?:public\s+)?cfg::node\b')
NODE_NAME_RE = re.compile(r'cfg::node\(\s*_this\s*,\s*"([^"]+)"\s*\)')
CLOSE_RE = re.compile(r'^\s*\}\s*(\w+)\s*\{\s*this\s*\}\s*;')


def parse_tree():
    lines = active_lines(read('rpcs3/Emu/system_config.h'))
    leaves = []
    stack = []  # [struct, yaml name, member]
    pending = []  # leaves of the open structs: (depth, leaf)
    for line in lines:
        m = STRUCT_RE.match(line)
        if m:
            if m.group(1) != 'cfg_root':
                stack.append({'name': None, 'leaves': []})
            continue
        m = NODE_NAME_RE.search(line)
        if m and stack and stack[-1]['name'] is None:
            stack[-1]['name'] = m.group(1)
            continue
        m = CLOSE_RE.match(line)
        if m and stack:
            node = stack.pop()
            for leaf in node['leaves']:
                leaf['member'] = m.group(1) + '.' + leaf['member']
                leaf['path'] = [node['name']] + leaf['path']
            (stack[-1]['leaves'] if stack else leaves).extend(node['leaves'])
            continue
        m = LEAF_RE.match(line)
        if not m:
            continue
        ctype, member, name, rest = m.group(1), m.group(2), c_string(m.group(3)), m.group(4).strip()
        args = split_args(rest.lstrip(',')) if rest.strip(', ') else []
        leaf = {'member': member, 'path': [name], 'ctype': ctype.replace(' ', ''), 'args': args}
        (stack[-1]['leaves'] if stack else leaves).append(leaf)
    for leaf in leaves:
        leaf['key'] = ' > '.join(leaf['path'])
        describe_leaf(leaf)
    return leaves


def describe_leaf(leaf):
    t, args = leaf['ctype'], leaf['args']
    m = re.match(r'cfg::(\w+)(?:<(.*)>)?$', t) if t != 'fifo_setting' else None
    kind = m.group(1) if m else '_enum'
    tmpl = m.group(2) if m else 'rsx_fifo_mode'
    leaf['enum'] = None
    lo = hi = None
    dyn_index = 1
    default = None
    if kind == '_bool':
        leaf['type'] = 'bool'
        default = args[0] if args else 'false'
    elif kind == '_enum':
        leaf['type'] = 'enum'
        leaf['enum'] = tmpl.strip()
        default = args[0] if args else None
    elif kind in ('_int', '_float', 'uint'):
        leaf['type'] = {'_int': 'int', '_float': 'float', 'uint': 'uint'}[kind]
        lo_e, hi_e = split_args(tmpl)
        lo, hi = eval_int(lo_e), eval_int(hi_e)
        default = args[0] if args else None
    elif kind in ('uint64', 'uint32', 'int32', 'int64'):
        leaf['type'] = 'uint' if kind.startswith('u') else 'int'
        bits = 64 if kind.endswith('64') else 32
        lo, hi = (0, 2 ** bits - 1) if kind.startswith('u') else (-2 ** (bits - 1), 2 ** (bits - 1) - 1)
        default = args[0] if args else None
    elif kind == 'uint128':
        leaf['type'] = 'uint128'
        default = args[0] if args else '0'
    elif kind == 'string':
        leaf['type'] = 'string'
        default = args[0] if args else '""'
    elif kind == 'set_entry':
        leaf['type'] = 'set'
        dyn_index = None
    elif kind in ('node_map_entry', 'map_entry', 'log_entry', 'device_entry'):
        leaf['type'] = {'node_map_entry': 'node_map', 'map_entry': 'map', 'log_entry': 'log', 'device_entry': 'device'}[kind]
        dyn_index = None
    else:
        raise SystemExit(f'unknown cfg type {t} for {leaf["key"]}')
    leaf['min'], leaf['max'] = lo, hi
    leaf['default_expr'] = default
    if dyn_index is None:
        leaf['dynamic'] = leaf['type'] in ('node_map', 'map', 'log', 'device')
    else:
        leaf['dynamic'] = len(args) > dyn_index and args[dyn_index].strip() == 'true'


def default_text(leaf, enums, spellings):
    """def_to_string() of the PS4 build (None when computed at run time, e.g. a random System Name)."""
    t, d = leaf['type'], leaf['default_expr']
    if t == 'bool':
        return 'true' if (d or 'false').strip() == 'true' else 'false'
    if t in ('set', 'node_map', 'map', 'log', 'device'):
        return ''
    if d is None:
        if t in ('int', 'float'):
            v = min(leaf['max'], max(leaf['min'], 0))
        elif t == 'uint':
            v = leaf['min']
        elif t == 'enum':
            v = 0
            return enum_spelling(leaf['enum'], v, enums, spellings)
        else:
            v = 0
        return fmt_number(t, v)
    d = d.strip()
    if t == 'string':
        return c_string(d) if d.startswith('"') else None
    if t == 'uint128':
        return None if '(' in d else '0x' + format(eval_int(d), '032X')
    if t == 'enum':
        m = re.match(r'(\w+)\{(\d+)\}$', d)
        if m:
            return enum_spelling(leaf['enum'], int(m.group(2)), enums, spellings)
        enumerator = d.split('::')[-1]
        return spellings.get(leaf['enum'], {}).get(enumerator)
    m = re.match(r'static_cast<\w+>\((\w+)::(\w+)\)$', d)
    if m:
        v = dict(enums.get(m.group(1), [])).get(m.group(2), 0)
        return fmt_number(t, v)
    return fmt_number(t, eval_int(d))


def fmt_number(t, v):
    if t == 'float':
        return format(float(v), '.17g')
    return str(int(v))


def enum_spelling(enum, value, enums, spellings):
    for name, v in enums.get(enum, []):
        if v == value:
            return spellings.get(enum, {}).get(name)
    return None


def enum_list(enum, enums, spellings):
    """cfg::_enum<T>::to_list(): the spellings of the values 0, 1, ... up to the first value without one."""
    by_value = {}
    for name, v in enums.get(enum, []):
        s = spellings.get(enum, {}).get(name)
        if s is not None and v not in by_value:
            by_value[v] = (name, s)
    out = []
    v = 0
    while v in by_value:
        out.append(by_value[v])
        v += 1
    return out


# ---------------------------------------------------------------- the PC settings dialog

def parse_ui():
    import xml.etree.ElementTree as ET
    root = ET.parse(os.path.join(REPO, 'rpcs3', 'rpcs3qt', 'settings_dialog.ui')).getroot()
    widgets = {}
    order = []

    def text_of(w, tag, names):
        for p in w.findall(tag):
            if p.get('name') in names:
                s = p.find('string')
                if s is not None and s.text is not None:
                    return s.text
        return None

    def walk(el, tab, groups, last_label):
        for ch in el:
            if ch.tag != 'widget':
                last_label = walk(ch, tab, groups, last_label)
                continue
            cls, name = ch.get('class'), ch.get('name')
            if cls == 'QWidget' and name and name.endswith('Tab'):
                t = text_of(ch, 'attribute', ('title',))
                if t:
                    walk(ch, t, [], None)
                    continue
            if cls == 'QGroupBox':
                walk(ch, tab, groups + [(name, text_of(ch, 'property', ('title',)))], None)
                continue
            if cls == 'QLabel':
                last_label = text_of(ch, 'property', ('text',))
            widgets[name] = {'class': cls, 'text': text_of(ch, 'property', ('text',)), 'tab': tab, 'groups': list(groups),
                             'label': last_label}
            order.append(name)
            last_label = walk(ch, tab, groups, last_label)
        return last_label

    walk(root, None, [], None)
    return widgets, order


def parse_tooltips():
    text = read('rpcs3/rpcs3qt/tooltips.h')
    out = {}
    for m in re.finditer(r'const QString\s+(\w+)\s*=\s*tr\(((?:\s*"(?:[^"\\]|\\.)*")+)\s*\)\s*;', text):
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))
        raw = ''.join(parts)
        out.setdefault(m.group(1), c_string('"' + raw + '"'))
    return out


def one_line(text):
    """A PC tooltip as one XMB line: its lines become ' · '-separated dialog rows; a leading '· ' bullet is dropped."""
    rows = [r.strip() for r in text.split('\n')]
    rows = [r[2:].strip() if r.startswith('· ') else r for r in rows]
    return ' · '.join(r for r in rows if r)


def parse_pc(leaves):
    by_member = {leaf['member']: leaf for leaf in leaves}
    loc = read('rpcs3/rpcs3qt/emu_settings_type.cpp')
    type_member = {m.group(1): m.group(2) for m in re.finditer(r'emu_settings_type::(\w+),\s*get_cfg_location\(local_cfg\.([\w.]+)\)', loc)}
    dlg = read('rpcs3/rpcs3qt/settings_dialog.cpp')
    type_widget, type_tip = {}, {}
    for m in re.finditer(r'Enhance\w+\(\s*emu_settings_type::(\w+)\s*,\s*ui->(\w+)\s*(?:,\s*(tooltips\.settings\.\w+|\{\}))?', dlg):
        type_widget.setdefault(m.group(1), m.group(2))
        if m.group(3) and m.group(3).startswith('tooltips'):
            type_tip.setdefault(m.group(1), m.group(3).split('.')[-1])
    for m in re.finditer(r'Enhance\w+\(\s*ui->(\w+)\s*,\s*emu_settings_type::(\w+)', dlg):
        type_widget.setdefault(m.group(2), m.group(1))
    widget_tip = {}
    for m in re.finditer(r'SubscribeTooltip\(\s*ui->(\w+)\s*,\s*tooltips\.settings\.(\w+)\s*\)', dlg):
        widget_tip.setdefault(m.group(1), m.group(2))
    widgets, order = parse_ui()
    tips = parse_tooltips()
    out = {}
    for t, member in type_member.items():
        leaf = by_member.get(member)
        if not leaf:
            continue
        w = type_widget.get(t)
        info = out.setdefault(leaf['key'], {'types': [], 'widget': None, 'tab': None, 'label': None, 'tip': None, 'group': None})
        info['types'].append(t)
        if w and w in widgets and not info['widget']:
            info['widget'] = w
        tip = type_tip.get(t) or (widget_tip.get(w) if w else None)
        if not tip and w and w in widgets:
            for gname, _ in reversed(widgets[w]['groups']):
                if gname in widget_tip:
                    tip = widget_tip[gname]
                    break
        if tip and tip in tips and not info['tip']:
            info['tip'] = tip
    # labels: a check box's text; else the title of the group box that holds only this setting; else the label before it
    setting_widgets = {i['widget'] for i in out.values() if i['widget']}
    group_count = {}
    for w in setting_widgets:
        for gname, _ in widgets[w]['groups']:
            group_count[gname] = group_count.get(gname, 0) + 1
    for key, info in out.items():
        w = info['widget']
        if not w:
            continue
        wi = widgets[w]
        info['tab'] = wi['tab']
        groups = wi['groups']
        info['group'] = groups[-1][1] if groups else None
        if wi['class'] in ('QCheckBox',) and wi['text']:
            info['label'] = wi['text']
        elif groups and group_count.get(groups[-1][0]) == 1 and groups[-1][1]:
            info['label'] = groups[-1][1]
        elif wi['label']:
            info['label'] = re.split(r':', wi['label'])[0].strip()
        elif groups:
            info['label'] = groups[-1][1]
    for key, (tab, label) in HAND_LABELS.items():
        info = out.setdefault(key, {'types': [], 'widget': None, 'tab': None, 'label': None, 'tip': None, 'group': None})
        info['tab'], info['label'] = tab, label
    for key, names in HAND_TIPS.items():
        if key in out and all(n in tips for n in names):
            out[key]['tip'] = '+'.join(names)
    for info in out.values():
        info['tip_text'] = ' · '.join(one_line(tips[n]) for n in info['tip'].split('+')) if info['tip'] else None
    return out, type_member


# Composite and sub-dialog controls of the PC dialog (no single widget text to read), radio button groups (titled by
# their group box), the two checkboxes whose PC types are swapped in emu_settings_type.cpp (the browser keys on the cfg
# name: each keeps its own checkbox text) and controls whose group holds labels the dialog rewrites at run time.
HAND_LABELS = {
    'Video > Relaxed ZCULL Sync': ('GPU', 'ZCULL Accuracy'),
    'Video > Accurate ZCULL stats': ('GPU', 'Accurate ZCULL stats'),
    'Core > MFC Commands Shuffling Limit': ('Advanced', 'Delay Each Odd MFC Command'),
    'Core > Libraries Control': ('Advanced', 'Firmware Libraries'),
    'Log': ('Debug', 'Log Levels'),
    'Video > Custom Anaglyph Matrices > Matrix Left': ('GPU', 'Left matrix'),
    'Video > Custom Anaglyph Matrices > Matrix Right': ('GPU', 'Right matrix'),
    'Audio > Microphone Type': ('Audio', 'Microphone Type'),
    'Audio > Microphone Devices': ('Audio', 'Microphone Devices'),
    'Input/Output > Emulated Midi devices': ('I/O', 'Emulated MIDI Devices'),
    'System > Console time offset (s)': ('System', 'Console Time'),
    'Video > Vulkan > Adapter': ('GPU', 'Graphics Device'),
    'Audio > Audio Device': ('Audio', 'Audio Device'),
    'Input/Output > Camera ID': ('I/O', 'Camera'),
    'Net > PSN Country': ('Network', 'Country'),
    'Audio > Audio Formats': ('Audio', 'Audio Formats'),
    'Miscellaneous > Window Title Format': ('Emulator', 'Game Window Title'),
    'Core > PPU Decoder': ('CPU', 'PPU Decoder'),
    'Core > SPU Decoder': ('CPU', 'SPU Decoder'),
    'Video > Shader Mode': ('GPU', 'Shader Mode'),
    'System > Enter button assignment': ('System', 'Enter Button Assignment'),
    'Video > FidelityFX CAS Sharpening Intensity': ('GPU', 'RCAS Sharpening Strength'),
    'Audio > Master Volume': ('Audio', 'Volume'),
    'Audio > Desired Audio Buffer Duration': ('Audio', 'Audio Buffer Duration'),
    'VFS > Disk cache maximum size (MB)': ('System', 'Disk Cache'),
    'Video > Performance Overlay > Horizontal Margin (%)': ('Emulator', 'Horizontal Margin'),
    'Video > Performance Overlay > Vertical Margin (%)': ('Emulator', 'Vertical Margin'),
    'Video > Performance Overlay > Center Horizontally': ('Emulator', 'Centered horizontally'),
    'Video > Performance Overlay > Center Vertically': ('Emulator', 'Centered vertically'),
    'Video > Disable Hardware Blending': ('Debug', 'Disable Hardware Blending'),
    'Video > Disable Hardware ColorSpace Remapping': ('Debug', 'Disable Hardware ColorSpace Remapping'),
}
HAND_TIPS = {
    'Video > Relaxed ZCULL Sync': ['zcull_operation_mode'],
    'Video > Accurate ZCULL stats': ['zcull_operation_mode'],
    'Core > Libraries Control': ['lle_list'],
    'Core > MFC Commands Shuffling Limit': ['mfc_delay_command'],
    'Core > Clocks scale': ['clocks_scale'],
    'Video > Vblank Rate': ['vblank_rate'],
    'Video > Driver Wake-Up Delay': ['wake_up_delay'],
    'Log': ['log_levels'],
    'Core > PPU Decoder': ['ppu__static', 'ppu_llvm'],
    'Core > SPU Decoder': ['spu__static', 'spu_dynamic', 'spu_asmjit', 'spu_llvm'],
    'Video > Shader Mode': ['legacy_shader_recompiler', 'async_shader_recompiler', 'async_with_shader_interpreter', 'shader_interpreter_only'],
    'System > Enter button assignment': ['enter_button_assignment'],
    'Video > Vulkan > Adapter': ['graphics_adapter'],
    'Audio > Audio Device': ['audio_device'],
    'Audio > Audio Formats': ['audio_format'],
    'Input/Output > Camera ID': ['camera_id'],
    'Input/Output > Emulated Midi devices': ['midi_devices'],
    'Audio > Microphone Devices': ['microphone'],
    'Net > PSN Country': ['psn_country'],
    'System > Console time offset (s)': ['console_time_offset'],
    'Miscellaneous > Window Title Format': ['game_window_title_format'],
    'Video > 3D Display Mode': ['stereo_render_mode'],
    'Video > Disable Hardware Blending': ['disable_hw_blending'],
    'Video > Disable Hardware ColorSpace Remapping': ['disable_hw_texel_remapping'],
}


def parse_values(type_member, leaves, enums, spellings):
    """GetLocalizedSetting: the PC text of each enum value, keyed by (setting key, YAML spelling)."""
    by_member = {leaf['member']: leaf for leaf in leaves}
    text = read('rpcs3/rpcs3qt/emu_settings.cpp')
    body = text[text.index('QString emu_settings::GetLocalizedSetting'):]
    body = body[:body.index('\n}\n')]
    body = '\n'.join(active_lines(body))
    out = {}
    for block in re.split(r'\n\tcase emu_settings_type::', body)[1:]:
        types = [block.split(':', 1)[0].strip()]
        member = type_member.get(types[0])
        leaf = by_member.get(member) if member else None
        if not leaf or leaf['type'] != 'enum':
            continue
        enum = leaf['enum']
        for m in re.finditer(r'case\s+([\w:]+)\s*:\s*return\s+tr\(\s*("(?:[^"\\]|\\.)*")', block):
            enumerator = m.group(1).split('::')[-1]
            spelling = spellings.get(enum, {}).get(enumerator)
            if spelling is None:
                continue
            out[(leaf['key'], spelling)] = (enum, enumerator, c_string(m.group(2)))
    # Two keys share one PC type (performance graph detail levels): the text applies to both
    for leaf in leaves:
        if leaf['type'] != 'enum':
            continue
        for other in leaves:
            if other is leaf or other['type'] != 'enum' or other['enum'] != leaf['enum']:
                continue
            for (k, s), v in list(out.items()):
                if k == other['key'] and (leaf['key'], s) not in out:
                    out[(leaf['key'], s)] = v
    return out


def parse_home_menu(leaves):
    by_member = {leaf['member']: leaf['key'] for leaf in leaves}
    text = read('rpcs3/Emu/RSX/Overlays/HomeMenu/overlay_home_menu_settings.cpp')
    keys = []
    for m in re.finditer(r'g_cfg\.([\w.]+)', text):
        member = m.group(1)
        for cut in (member, member.rsplit('.', 1)[0]):
            if cut in by_member and by_member[cut] not in keys:
                keys.append(by_member[cut])
                break
    return keys


# ---------------------------------------------------------------- output

def c_quote(s):
    out = '"'
    for ch in s:
        if ch == '"' or ch == '\\':
            out += '\\' + ch
        elif ord(ch) < 0x20:
            out += '\\x%02x' % ord(ch)
        else:
            out += ch
    return out + '"'


def num_literal(t, v):
    if v is None:
        return '0'
    if t == 'float':
        return repr(float(v))
    if v < 0:
        return '(%dLL)' % v if v > -2 ** 63 else '(-9223372036854775807LL - 1)'
    return '%dULL' % v


HEADER = '// Generated by ps4-port/tools/gen_cfg_catalog.py from {sources}. Do not edit; run the generator.\n'


def gen_tree(leaves, enums, spellings):
    lines = [HEADER.format(sources='rpcs3/Emu/system_config.h (PS4 + LLVM + Vulkan branch)'),
             '// CFG_LEAF(key, cfg type, min, max, default (def_to_string; nullptr = computed at run time), dynamic)']
    for leaf in leaves:
        d = default_text(leaf, enums, spellings)
        lines.append('CFG_LEAF(%s, %s, %s, %s, %s, %s)' % (c_quote(leaf['key']), leaf['type'], num_literal(leaf['type'], leaf['min']),
                     num_literal(leaf['type'], leaf['max']), 'nullptr' if d is None else c_quote(d), 'true' if leaf['dynamic'] else 'false'))
    return '\n'.join(lines) + '\n'


def gen_enum_names(leaves, enums, spellings):
    lines = [HEADER.format(sources='system_config_types.cpp, cellSysutil.cpp and KeyboardHandler.cpp'),
             '// CFG_MOCK_ENUM(T, {value, "YAML spelling"}, ...): every enumerator with a spelling, PS4 build branch']
    seen = []
    for leaf in leaves:
        e = leaf['enum']
        if not e or e in seen:
            continue
        seen.append(e)
        items = []
        for name, v in enums.get(e, []):
            s = spellings.get(e, {}).get(name)
            if s is None:
                continue
            value = '%s::%s' % (e, name) if not e.startswith('Cell') else '%s(%d)' % (e, v)
            items.append('{%s, %s}' % (value, c_quote(s)))
        lines.append('CFG_MOCK_ENUM(%s, %s)' % (e, ', '.join(items)))
    return '\n'.join(lines) + '\n'


def gen_pc_text(leaves, pc, values, home):
    lines = [HEADER.format(sources='the PC settings dialog (settings_dialog.ui/.cpp, emu_settings.cpp, tooltips.h)'),
             '// CFG_PC_LABEL(key, PC tab, English label): the text of the PC control (or the cfg name for a YAML-only setting)',
             '// CFG_PC_VALUE(key, YAML spelling, English label): emu_settings::GetLocalizedSetting',
             '// CFG_PC_TIP(key, English tooltip as one line, rows joined by " · ")',
             '// CFG_HOME_MENU(key): the in-game home menu (Options+Touchpad) edits it while a game runs']
    for leaf in leaves:
        info = pc.get(leaf['key'])
        tab = (info or {}).get('tab') or 'YAML'
        label = (info or {}).get('label') or leaf['path'][-1]
        lines.append('CFG_PC_LABEL(%s, %s, %s)' % (c_quote(leaf['key']), c_quote(tab), c_quote(label)))
    for (key, spelling), (_, _, text) in sorted(values.items()):
        lines.append('CFG_PC_VALUE(%s, %s, %s)' % (c_quote(key), c_quote(spelling), c_quote(text)))
    for leaf in leaves:
        info = pc.get(leaf['key'])
        if info and info.get('tip_text'):
            lines.append('CFG_PC_TIP(%s, %s)' % (c_quote(leaf['key']), c_quote(info['tip_text'])))
    for key in home:
        lines.append('CFG_HOME_MENU(%s)' % c_quote(key))
    return '\n'.join(lines) + '\n'


def ident(member):
    return member.replace('.', '_').upper()


def gen_skeleton(leaves, pc):
    tabs = {'CPU': 'cpu', 'GPU': 'gpu', 'Audio': 'audio', 'I/O': 'io', 'System': 'system', 'Network': 'network',
            'Advanced': 'advanced', 'Emulator': 'emulator', 'Debug': 'debug'}
    node_tab = {'Core': 'advanced', 'Video': 'gpu', 'Audio': 'audio', 'Input/Output': 'io', 'System': 'system', 'VFS': 'system',
                'Net': 'network', 'Savestate': 'emulator', 'Miscellaneous': 'emulator', 'Log': 'debug'}
    lines = ['// Skeleton written by ps4-port/tools/gen_cfg_catalog.py --skeleton: tabs and labels of the PC dialog; class E,',
             '// sections, help ids and editor hints still to be chosen by hand.']
    for leaf in leaves:
        info = pc.get(leaf['key']) or {}
        tab = tabs.get(info.get('tab') or '', node_tab.get(leaf['path'][0], 'emulator'))
        i = ident(leaf['member'])
        lines.append('CFG_ITEM(%s, %s, CFGG_YAML, E, CFGL_%s, CFGH_%s, count_, count_, auto_, 0, 0, "", "", "", both, 0)' %
                     (c_quote(leaf['key']), tab, i, i))
    return '\n'.join(lines) + '\n'


def gen_strings(leaves, pc, values):
    lines = ['// English-only skeleton written by ps4-port/tools/gen_cfg_catalog.py --strings']
    for leaf in leaves:
        info = pc.get(leaf['key']) or {}
        i = ident(leaf['member'])
        lines.append('I18N(CFGL_%s, "", %s, "", "")' % (i, c_quote(info.get('label') or leaf['path'][-1])))
        if info.get('tip_text'):
            lines.append('I18N(CFGTIP_%s, "", %s, "", "")' % (i, c_quote(info['tip_text'])))
    for (key, spelling), (enum, enumerator, text) in sorted(values.items()):
        lines.append('// %s = %s' % (key, spelling))
        lines.append('I18N(CFGV_%s_%s, "", %s, "", "")' % (enum.upper(), enumerator.lstrip('_').upper(), c_quote(text)))
    return '\n'.join(lines) + '\n'


def catalog_keys():
    if not os.path.isfile(CATALOG):
        return None
    with open(CATALOG, encoding='utf-8') as f:
        text = f.read()
    return re.findall(r'^CFG_ITEM\(\s*("(?:[^"\\]|\\.)*")', text, re.M)


def load_all():
    enums = {}
    for rel in ('rpcs3/Emu/system_config_types.h', 'rpcs3/Emu/Cell/Modules/cellSysutil.h', 'rpcs3/Emu/Io/Keyboard.h'):
        enums.update(parse_enums(read(rel)))
    spellings = {}
    for rel in ('rpcs3/Emu/system_config_types.cpp', 'rpcs3/Emu/Cell/Modules/cellSysutil.cpp', 'rpcs3/Emu/Io/KeyboardHandler.cpp'):
        spellings.update(parse_spellings(read(rel)))
    leaves = parse_tree()
    pc, type_member = parse_pc(leaves)
    values = parse_values(type_member, leaves, enums, spellings)
    home = parse_home_menu(leaves)
    return leaves, enums, spellings, pc, values, home


def main(argv):
    leaves, enums, spellings, pc, values, home = load_all()
    problems = []
    keys = [leaf['key'] for leaf in leaves]
    if len(set(keys)) != len(keys):
        problems.append('duplicate keys in cfg_root')
    for leaf in leaves:
        if any(' > ' in p for p in leaf['path']):
            problems.append('a node name contains " > ": ' + leaf['key'])
        if leaf['enum'] and not enum_list(leaf['enum'], enums, spellings):
            problems.append('no spellings for enum %s (%s)' % (leaf['enum'], leaf['key']))
    generated = {
        os.path.join(MOCKS, 'cfg_tree.inc'): gen_tree(leaves, enums, spellings),
        os.path.join(MOCKS, 'cfg_enum_names.inc'): gen_enum_names(leaves, enums, spellings),
        os.path.join(MOCKS, 'cfg_pc_text.inc'): gen_pc_text(leaves, pc, values, home),
    }
    if '--json' in argv:
        with open(argv[argv.index('--json') + 1], 'w', encoding='utf-8') as f:
            json.dump({'leaves': leaves, 'pc': pc, 'values': {k[0] + ' = ' + k[1]: v for k, v in values.items()}, 'home': home,
                       'enums': {e: enum_list(e, enums, spellings) for e in sorted({l['enum'] for l in leaves if l['enum']})}},
                      f, ensure_ascii=False, indent=1)
    if '--skeleton' in argv:
        with open(argv[argv.index('--skeleton') + 1], 'w', encoding='utf-8', newline='\n') as f:
            f.write(gen_skeleton(leaves, pc))
    if '--strings' in argv:
        with open(argv[argv.index('--strings') + 1], 'w', encoding='utf-8', newline='\n') as f:
            f.write(gen_strings(leaves, pc, values))
    if '--check' in argv:
        for path, text in generated.items():
            old = open(path, encoding='utf-8').read() if os.path.isfile(path) else None
            if old != text:
                problems.append('stale generated file (run the generator): ' + os.path.relpath(path, REPO))
        listed = catalog_keys()
        if listed is None:
            problems.append('missing ' + os.path.relpath(CATALOG, REPO))
        else:
            listed = [c_string(k) for k in listed]
            for k in keys:
                if k not in listed:
                    problems.append('setting without a catalog row: ' + k)
            for k in listed:
                if k not in keys:
                    problems.append('catalog row without a setting: ' + k)
            if len(set(listed)) != len(listed):
                problems.append('duplicate catalog rows')
    else:
        os.makedirs(MOCKS, exist_ok=True)
        for path, text in generated.items():
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
    print('gen_cfg_catalog: %d settings, %d PC labels, %d PC value labels, %d PC tooltips, %d home menu settings' %
          (len(leaves), sum(1 for i in pc.values() if i.get('label')), len(values), sum(1 for i in pc.values() if i.get('tip_text')), len(home)))
    for p in problems:
        print('gen_cfg_catalog: ' + p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
