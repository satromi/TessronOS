#!/usr/bin/env python3
#
# gen_hw.py -- the chip tables of the x64 PC's LAN drivers, made from the release's sources
#
# Copyright (C) 2026 satromi
# This software is distributed under the T-License 2.2.
#
# The per-revision register tables, MCU images and bring-up sequences of the
# RTL8125, RTL8152/8153, AX88179 and AX88772 are not typed by hand: this takes
# them, unchanged, from the OpenBSD 8.0 sys.tar.gz that tools/openbsd/build.sh
# keeps (make sure ~/openbsd/dl/8.0/sys.tar.gz is there), and writes
#
#	device/net/x86_64/rge_hw.h
#	device/usb/x86_64/ure_hw.h
#	device/usb/x86_64/axen_reg.h
#	device/usb/x86_64/axe_reg.h
#
# with the licence text each of those files came under. Run it again only to
# take a newer release; the output is kept in the tree and the build does not
# run it.
#
#	python3 tools/x64/nic/gen_hw.py [sys.tar.gz]

import os, re, sys, tarfile

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
TAR = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/openbsd/dl/8.0/sys.tar.gz')

names = {'rgereg': 'sys/dev/pci/if_rgereg.h', 'rge': 'sys/dev/pci/if_rge.c',
         'urereg': 'sys/dev/usb/if_urereg.h', 'ure': 'sys/dev/usb/if_ure.c',
         'axenreg': 'sys/dev/usb/if_axenreg.h', 'axen': 'sys/dev/usb/if_axen.c',
         'axereg': 'sys/dev/usb/if_axereg.h', 'axe': 'sys/dev/usb/if_axe.c', 'usbdevs': 'sys/dev/usb/usbdevs'}
text = {}
with tarfile.open(TAR) as t:
    for m in t:
        n = m.name.lstrip('./')
        for k, v in names.items():
            if n == v:
                text[k] = t.extractfile(m).read().decode('latin-1')
missing = set(names) - set(text)
if missing:
    sys.exit('not in %s: %s' % (TAR, missing))


def licence(src):
    """The comment that carries the file's copyright, whole"""
    m = re.search(r'/\*(?:(?!\*/).)*?Copyright.*?\*/', src, re.S)
    return m.group(0) + '\n'


def func(src, name):
    """One function of the source, with the line of its return type"""
    m = re.search(r'^(?:[\w ]+\*?\n)' + name + r'\(.*?^\}\n', src, re.S | re.M)
    if m is None:
        sys.exit('no function ' + name)
    return m.group(0)


def table(src, decl):
    """One of the register tables declared together, as a table of its own"""
    m = re.search(r'^\},? ?' + decl + r'\[\] = \{\n(.*?)^\}', src, re.S | re.M)
    if m is None:
        sys.exit('no table ' + decl)
    return 'static const struct rge_regval ' + decl + '[] = {\n' + m.group(1) + '};\n\n'


def macro(src, name):
    """A #define with its continuation lines"""
    m = re.search(r'^#define\s+' + name + r'\b(?:.*\\\n)*.*\n', src, re.M)
    if m is None:
        sys.exit('no macro ' + name)
    return m.group(0)


def defines(src, stop):
    """Every #define of a register header before the mark"""
    head = src[:src.index(stop)]
    return ''.join(m.group(0) for m in re.finditer(r'^#\s*define\s+\w+(?:.*\\\n)*.*\n', head, re.M))


def write(path, parts):
    with open(os.path.join(TOP, path), 'w', newline='\n') as f:
        f.write(''.join(parts))
    print(path)


NOTE = '/* Made by tools/x64/nic/gen_hw.py: not to be edited. */\n\n'

# ---------------------------------------------------------------- RTL8125, RTL8125B and RTL8125D
r, c = text['rgereg'], text['rge']
head = r[:r.index('struct rge_softc {')]
head = ''.join(m.group(0) for m in re.finditer(r'^#\s*define\s+\w+(?:.*\\\n)*.*\n', head, re.M))
# what names a type or a helper of the source's own kernel is not needed here
head = ''.join(l for l in head.splitlines(True) if 'PCI_MAPREG' not in l and 'RGE_ADDR_' not in l)
rge_parts = [licence(c), '\n', NOTE, head, '\nstruct rge_regval {\n\tuint16_t reg;\n\tuint16_t val;\n};\n\n']
rge_parts += [table(r, n) for n in ('rtl8125_mac_bps', 'rtl8125b_mac_bps', 'rtl8125d_2_mac_bps', 'mac_r25_ephy', 'mac_r25b_ephy', 'mac_r25d_2_mcu')]
rge_parts += [macro(r, 'MAC_R25_MCU'), macro(r, 'MAC_R25B_MCU'), macro(r, 'MAC_R25D_MCU')]
rge_parts += ['\nstatic const struct rge_regval mac_r25_mcu[] = {\n\tMAC_R25_MCU\n}, mac_r25b_mcu[] = {\n\tMAC_R25B_MCU\n}, mac_r25d_1_mcu[] = {\n\tMAC_R25D_MCU\n};\n\n']
rge_parts += ['/* The bring-up of the revisions */\n']
for n in ('rge_ephy_config_mac_r25', 'rge_ephy_config_mac_r25b', 'rge_phy_config_mac_r25', 'rge_phy_config_mac_r25b',
          'rge_phy_config_mac_r25d_1', 'rge_phy_config_mac_r25d_2'):
    rge_parts += ['static __inline__ ' + func(c, n), '\n']
write('device/net/x86_64/rge_hw.h', rge_parts)

# ---------------------------------------------------------------- RTL8152, RTL8153, RTL8153B
r, c = text['urereg'], text['ure']
ure_parts = [licence(c), '\n', NOTE, defines(r, 'struct ure_chain {'), '\n']
ure_parts += [macro(r, 'URE_FLAG_' + f) for f in ('LINK', '8152', '8153B', '8156', '8156B', '8157', 'CHIP_MASK')]
ure_parts += [macro(r, 'URE_CHIP_VER_' + f) for f in ('4C00', '4C10', '5C00', '5C10', '5C20', '5C30', '6010', '7420')]
ure_parts += [macro(c, 'URE_' + f) for f in ('SETBIT_1', 'SETBIT_2', 'SETBIT_4', 'CLRBIT_1', 'CLRBIT_2', 'CLRBIT_4',
                                            'OCP_CMD_SETBIT', 'OCP_CMD_CLRBIT')]
ure_parts += ['\n']
for n in ('ure_read_mem', 'ure_write_mem', 'ure_read_1', 'ure_read_2', 'ure_read_4', 'ure_write_1', 'ure_write_2',
          'ure_write_4', 'ure_ocp_reg_read', 'ure_ocp_reg_write', 'ure_ocp_cmd_read', 'ure_ocp_cmd_write',
          'ure_rxvlan', 'ure_reset', 'ure_wait_for_flash', 'ure_reset_bmu', 'ure_disable_teredo', 'ure_rtl8153_phy_status',
          'ure_rtl8152_init', 'ure_rtl8153_init', 'ure_rtl8153b_init', 'ure_rtl8152_nic_reset',
          'ure_rtl8153_nic_reset', 'ure_ifmedia_init'):
    ure_parts += ['static __inline__ ' + func(c, n), '\n']
write('device/usb/x86_64/ure_hw.h', ure_parts)

# ---------------------------------------------------------------- AX88179
r, c = text['axenreg'], text['axen']
write('device/usb/x86_64/axen_reg.h', [licence(c), '\n', NOTE, defines(r, 'struct axen_softc;')])

# ---------------------------------------------------------------- AX88772, AX88772A, AX88772B
r, c, u = text['axereg'], text['axe'], text['usbdevs']


def usbdev(kind, *name):
    """A vendor's or a product's number as usbdevs gives it"""
    m = re.search(r'^' + kind + r'\s+' + r'\s+'.join(name) + r'\s+(0x[0-9a-fA-F]{4})\b', u, re.M)
    if m is None:
        sys.exit('not in usbdevs: %s %s' % (kind, ' '.join(name)))
    return m.group(1).lower()


# the entries of axe_devs[] that have AX772 among their flags: the AX88772 family
devs = re.search(r'^const struct axe_type axe_devs\[\] = \{\n(.*?)^\};', c, re.S | re.M)
if devs is None:
    sys.exit('no axe_devs')
lines = []
for m in re.finditer(r'\{\s*\{\s*USB_VENDOR_(\w+)\s*,\s*USB_PRODUCT_(\w+)\s*\}\s*,\s*([^}]*?)\s*\}', devs.group(1)):
    vendor, product, flags = m.group(1), m.group(2), m.group(3)
    if 'AX772' not in re.findall(r'\w+', flags):
        continue
    if not product.startswith(vendor + '_'):
        sys.exit('product %s is not of vendor %s' % (product, vendor))
    lines.append('\tAXE_772_DEV(%s, %s, %s)\t/* %s %s */ \\\n' % (
        usbdev('vendor', vendor), usbdev('product', vendor, product[len(vendor) + 1:]),
        ' | '.join(f.strip() for f in flags.split('|')), vendor, product[len(vendor) + 1:]))
if not lines:
    sys.exit('no AX772 entry in axe_devs')
axe_parts = [licence(r), '\n', licence(c), '\n', NOTE, defines(r, 'struct axe_type {'), '\n']
axe_parts += [macro(r, f) for f in ('AX178', 'AX772', 'AX772B')]
axe_parts += ['\n/*\n * The entries of axe_devs[] in if_axe.c whose flags hold AX772, with the\n'
              ' * numbers of sys/dev/usb/usbdevs. AXE_772_DEV(vendor, product, flags) is\n'
              ' * defined by the file that takes the list.\n */\n#define AXE_772_DEVS \\\n']
axe_parts += lines + ['\t/* end */\n']
write('device/usb/x86_64/axe_reg.h', axe_parts)
