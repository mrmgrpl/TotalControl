"""
Extract CrSDK property and command data from headers + HTML docs.
Outputs C++ tables for CommandHandler.cpp.
"""
import sys, io, re
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

PROJ  = Path('D:/Dev/TotalControl')
SDK   = PROJ / 'external/CrSDK/include'
DOCS  = Path('D:/Dev/external/CrSDK/CrSDK_API_Reference_v2.01.00/html')
DP    = DOCS / 'dp'
CMD   = DOCS / 'command'

# ── 1. Compute all property codes from the enum ──────────────────────────────

hdr = (SDK / 'CrDeviceProperty.h').read_text(encoding='utf-8', errors='replace')

# Extract the enum body
m = re.search(r'enum CrDevicePropertyCode\s*:\s*CrInt32u\s*\{(.+?)\};', hdr, re.DOTALL)
enum_body = m.group(1)

prop_codes = {}  # name -> int code
cur = 0

for line in enum_body.split('\n'):
    line = line.strip()
    if not line or line.startswith('//') or line.startswith('/*') or line.startswith('*'):
        continue
    # Strip inline comments
    line = re.sub(r'/\*.*?\*/', '', line).strip()
    line = re.sub(r'//.*', '', line).strip()
    # Strip trailing comma
    line = line.rstrip(',').strip()
    if not line:
        continue

    # Handle assignment: NAME = VALUE or NAME = OTHER_NAME
    if '=' in line:
        parts = line.split('=', 1)
        name = parts[0].strip()
        val_str = parts[1].strip()
        if val_str.startswith('0x'):
            cur = int(val_str, 16)
        elif val_str.isdigit():
            cur = int(val_str)
        elif val_str in prop_codes:
            cur = prop_codes[val_str]
        else:
            # aliased to another name - keep cur
            pass
        if name.startswith('CrDeviceProperty_'):
            prop_codes[name] = cur
            cur += 1
    else:
        name = line
        if name.startswith('CrDeviceProperty_'):
            prop_codes[name] = cur
            cur += 1

print(f'[info] Computed {len(prop_codes)} property codes')

# ── 2. Parse main table for access modes ─────────────────────────────────────

list_html = (DOCS / 'function_list' / 'device_property_list.html').read_text(encoding='utf-8', errors='replace')

# Extract rows from the table
prop_mode = {}  # name -> mode string (e.g. 'R', 'R/W', 'R/T')
rows = re.findall(r'<tr[^>]*>(.*?)</tr>', list_html, re.DOTALL)
for row in rows:
    # Get all <td> cells
    cells = re.findall(r'<td[^>]*>(.*?)</td>', row, re.DOTALL)
    if len(cells) < 4:
        continue
    # Second cell has property name link
    name_match = re.search(r'CrDeviceProperty_(\w+)', cells[1])
    if not name_match:
        continue
    name = 'CrDeviceProperty_' + name_match.group(1)
    # Fourth cell has mode
    mode_text = re.sub(r'<[^>]+>', '', cells[3]).strip()
    prop_mode[name] = mode_text

print(f'[info] Got access modes for {len(prop_mode)} properties')

# ── 3. Parse individual dp_*.html for data types and enum values ─────────────

def strip_tags(s):
    return re.sub(r'<[^>]+>', '', s).strip()

def parse_dp(path):
    """Returns (datatype_str, {enum_name: explanation})"""
    content = path.read_text(encoding='utf-8', errors='replace')
    # Find data type
    dt_match = re.search(r'CrDataType_(\w+)', content)
    datatype = dt_match.group(1) if dt_match else 'Unknown'

    # Find value table rows
    values = {}
    main_start = content.find('role="main"')
    if main_start == -1:
        return datatype, values
    main_content = content[main_start:main_start+20000]
    rows = re.findall(r'<tr[^>]*>(.*?)</tr>', main_content, re.DOTALL)
    for row in rows:
        cells = re.findall(r'<t[dh][^>]*>(.*?)</t[dh]>', row, re.DOTALL)
        if len(cells) >= 2:
            code_cell = strip_tags(cells[0])
            expl_cell = strip_tags(cells[1])
            if code_cell and code_cell != 'Parameter Code' and code_cell != 'Explanation':
                values[code_cell] = expl_cell
    return datatype, values

# Parse all dp files
prop_dtype = {}  # name -> datatype string
prop_values = {} # name -> {enum_name: explanation}

for dp_file in sorted(DP.glob('dp_*.html')):
    # Map filename to property name
    stem = dp_file.stem  # e.g. 'dp_ShutterSpeed'
    prop_name_part = stem[3:]  # 'ShutterSpeed'
    full_name = 'CrDeviceProperty_' + prop_name_part

    # Some files map to slightly different names; find best match
    if full_name not in prop_codes:
        # Try case-insensitive match
        candidates = [k for k in prop_codes if k.lower() == full_name.lower()]
        if candidates:
            full_name = candidates[0]
        # Otherwise skip (it's a sub-property page)

    dt, vals = parse_dp(dp_file)
    prop_dtype[full_name] = dt
    if vals:
        prop_values[full_name] = vals

print(f'[info] Parsed data types for {len(prop_dtype)} properties')

# ── 4. Parse CrDeviceProperty.h for key enum values (per-property) ───────────

# Extract named value enums (e.g. CrExposureProgram_M = 2)
value_enums = {}  # prefix -> {name: int_value}
cur = 0

for line in hdr.split('\n'):
    stripped = line.strip().rstrip(',')
    # Start of a new enum
    em = re.match(r'enum\s+(Cr\w+)\s*(?::\s*\w+)?\s*\{', stripped)
    if em:
        enum_name = em.group(1)
        if enum_name != 'CrDevicePropertyCode':
            value_enums[enum_name] = {}
            cur = 0
        continue
    # Enum entries
    if stripped.startswith('Cr') and '=' in stripped:
        parts = stripped.split('=', 1)
        name = re.sub(r'/\*.*?\*/', '', parts[0]).strip()
        val_s = re.sub(r'//.*', '', re.sub(r'/\*.*?\*/', '', parts[1])).strip().rstrip(',')
        try:
            cur = int(val_s, 16) if '0x' in val_s else int(val_s)
        except:
            pass
        # Store under the enum prefix
        for k in value_enums:
            if name.startswith(k.replace('Cr', 'Cr')):
                value_enums[k][name] = cur
                break

print(f'[info] Extracted {len(value_enums)} value enums')

# ── 5. Parse commands from CrControlCode.h ───────────────────────────────────

ctrl_hdr = (SDK / 'CrControlCode.h').read_text(encoding='utf-8', errors='replace')
cmd_codes = {}  # name -> int code
cur = 0
for line in ctrl_hdr.split('\n'):
    stripped = line.strip().rstrip(',')
    stripped = re.sub(r'/\*.*?\*/', '', stripped).strip()
    m = re.match(r'(CrCommandId_\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)', stripped)
    if m:
        cmd_codes[m.group(1)] = int(m.group(2), 0)

print(f'[info] Extracted {len(cmd_codes)} command codes')

# ── 6. Output key properties table for CommandHandler ────────────────────────

# Properties we care about for the property registry (tc set/get)
# These are the main photographic/camera control properties
KEY_PROPS = [
    # Name suffix           cli-name              writable
    ('FNumber',             'f-number',            True),
    ('ExposureBiasCompensation', 'ev-comp',        True),
    ('ShutterSpeed',        'shutter-speed',       True),
    ('IsoSensitivity',      'iso',                 True),
    ('ExposureProgramMode', 'exposure-mode',       True),
    ('FileType',            'file-type',           True),
    ('StillImageQuality',   'jpeg-quality',        True),
    ('WhiteBalance',        'white-balance',       True),
    ('FocusMode',           'focus-mode',          True),
    ('MeteringMode',        'metering-mode',       True),
    ('FlashMode',           'flash-mode',          True),
    ('DriveMode',           'drive-mode',          True),
    ('DRO',                 'dro',                 True),
    ('ImageSize',           'image-size',          True),
    ('AspectRatio',         'aspect-ratio',        True),
    ('PictureEffect',       'picture-effect',      True),
    ('FocusArea',           'focus-area',          True),
    ('Colortemp',           'color-temp',          True),
    ('ColorTuningAB',       'color-tuning-ab',     True),
    ('ColorTuningGM',       'color-tuning-gm',     True),
    ('LiveViewDisplayEffect','lv-display-effect',  True),
    ('StillImageStoreDestination','store-dest',    True),
    ('PriorityKeySettings', 'priority-key',        True),
    ('AFTrackingSensitivity','af-tracking',        True),
    ('AF_Area_Position',    'af-area-pos',         True),
    ('Zoom_Scale',          'zoom-scale',          False),
    ('Zoom_Setting',        'zoom-setting',        True),
    ('Zoom_Operation',      'zoom-op',             True),
    ('Movie_File_Format',   'movie-format',        True),
    ('Movie_Recording_Setting','movie-rec-setting',True),
    ('Movie_Recording_FrameRateSetting','movie-fps',True),
    ('CompressionFileFormatStill','compression',   True),
    ('MediaSLOT1_FileType', 'slot1-filetype',      True),
    ('MediaSLOT2_FileType', 'slot2-filetype',      True),
    ('MediaSLOT1_ImageSize','slot1-imgsize',       True),
    ('MediaSLOT2_ImageSize','slot2-imgsize',       True),
    ('RAW_FileCompressionType','raw-compression',  True),
    ('IrisModeSetting',     'iris-mode',           True),
    ('ShutterModeSetting',  'shutter-mode-set',    True),
    ('GainControlSetting',  'gain-control',        True),
    ('PlaybackMedia',       'playback-media',      True),
    ('TouchOperation',      'touch-op',            True),
    ('SelectFinder',        'finder',              True),
    ('BodyKeyLock',         'key-lock',            True),
    ('ExposureCtrlType',    'exposure-ctrl',       True),
    ('ShutterSlow',         'shutter-slow',        True),
    ('SubjectRecognitionAF','subject-recog',       True),
    ('AFTransitionSpeed',   'af-transition-spd',   True),
    ('AFSubjShiftSens',     'af-subj-shift',       True),
    ('NDFilter',            'nd-filter',           True),
    ('AWB',                 'awb-lock',            True),
    # Status / read-only
    ('BatteryRemainDisplayUnit','battery-unit',    False),
    ('PowerSource',         'power-source',        False),
    ('FocusModeStatus',     'focus-mode-status',   False),
]

print('\n// ─── Property registry ──────────────────────────────────────────────────────')
print('// { cli-name, property-code, datatype-code, writable }')
print('static const PropDef kProps[] = {')

for (suffix, cli, writable) in KEY_PROPS:
    full = 'CrDeviceProperty_' + suffix
    code = prop_codes.get(full, -1)
    if code == -1:
        print(f'    // NOT FOUND: {full}')
        continue
    dt = prop_dtype.get(full, 'Unknown')
    # Map to CrDataType code
    dt_map = {
        'UInt8Array': 'CrDataType_UInt8Array',
        'UInt8': 'CrDataType_UInt8',
        'UInt16Array': 'CrDataType_UInt16Array',
        'UInt16': 'CrDataType_UInt16',
        'UInt32Array': 'CrDataType_UInt32Array',
        'UInt32': 'CrDataType_UInt32',
        'Int8': 'CrDataType_Int8',
        'Int16': 'CrDataType_Int16',
        'Int32': 'CrDataType_Int32',
        'Int64': 'CrDataType_Int64',
        'STR': 'CrDataType_STR',
        'Unknown': '0',
    }
    dt_code = dt_map.get(dt, f'0 /* {dt} */')
    w = 'true' if writable else 'false'
    print(f'    {{ L"{cli}", {hex(code)}, {dt_code}, {w} }},  // {full}')

print('};')

# ── 7. Output command codes ───────────────────────────────────────────────────
print('\n// ─── Command codes ───────────────────────────────────────────────────────────')
for name, code in sorted(cmd_codes.items(), key=lambda x: x[1]):
    print(f'    // {hex(code)}  {name}')

print('\n[done]')
