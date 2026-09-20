# Deliberately standalone; not subdir()'d by the shipping DXVK build.
project('warvk-render-host-lab', 'cpp', version : '0.1.0',
  default_options : ['cpp_std=c++17', 'warning_level=3'])
protocol_test = executable('warvk_host_protocol_test',
  ['protocol.cpp', '../../AutoTest/test_render_host_protocol.cpp'], install : false)
test('rh0_codec_session', protocol_test, timeout : 30)
slot_test = executable('warvk_host_slot_test',
  ['protocol.cpp', 'slot_ledger.cpp', '../../AutoTest/test_render_host_slot_ledger.cpp'], install : false)
test('rh1_slot_core', slot_test, timeout : 30)
wire_test = executable('warvk_host_slot_wire_test',
  ['protocol.cpp', 'slot_wire.cpp', '../../AutoTest/test_render_host_slot_wire.cpp'], install : false)
test('rh1_slot_wire', wire_test, timeout : 30)
inbox_test = executable('warvk_host_producer_inbox_test',
  '../../AutoTest/test_render_host_producer_inbox.cpp',
  dependencies : dependency('threads'), install : false)
test('producer_inbox_core', inbox_test, timeout : 30)
envelope_test = executable('warvk_host_sample_envelope_test',
  ['protocol.cpp', 'slot_ledger.cpp', 'sample_envelope.cpp', '../../AutoTest/test_render_host_sample_envelope.cpp'],
  install : false)
test('sample_envelope_core', envelope_test, timeout : 30)

if host_machine.system() == 'windows'
  cpp = meson.get_compiler('cpp')
  win_deps = [cpp.find_library('bcrypt'), cpp.find_library('advapi32')]
  transport_sources = ['protocol.cpp', 'win32_transport.cpp', 'win32_security.cpp']
  shared_sources = transport_sources + ['slot_ledger.cpp', 'slot_wire.cpp', 'shared_slots.cpp']
  unicode_args = cpp.get_id() in ['gcc', 'clang'] ? ['-municode'] : []
  random_test = executable('warvk_host_random_test',
    transport_sources + ['../../AutoTest/test_render_host_random.cpp'],
    dependencies : win_deps, install : false)
  test('rh0_rng_lifetime', random_test, timeout : 10)
  if host_machine.cpu_family() == 'x86_64'
    executable('warvk_host_cpu_lab', transport_sources + ['host_main.cpp'],
      dependencies : win_deps, link_args : unicode_args, install : false)
    executable('warvk_host_shared_lab', shared_sources + ['slot_host_main.cpp', 'slot_host_runtime.cpp'],
      dependencies : win_deps, link_args : unicode_args, install : false)
    executable('warvk_host_sample_lab', shared_sources + ['slot_host_runtime.cpp', 'sample_envelope.cpp', 'sample_host_main.cpp'],
      dependencies : win_deps, link_args : unicode_args, install : false)
  elif host_machine.cpu_family() == 'x86'
    executable('warvk_host_transport_client_test',
      transport_sources + ['owned_child.cpp', '../../AutoTest/test_render_host_transport.cpp'],
      dependencies : win_deps, link_args : unicode_args, install : false)
    executable('warvk_host_shared_client_test',
      shared_sources + ['owned_child.cpp', '../../AutoTest/test_render_host_shared_slots.cpp'],
      dependencies : win_deps, link_args : unicode_args, install : false)
    executable('warvk_host_sample_client_test',
      shared_sources + ['owned_child.cpp', 'sample_envelope.cpp', '../../AutoTest/test_render_host_sample_worker.cpp'],
      dependencies : win_deps + [dependency('threads')], link_args : unicode_args, install : false)
  endif
  # Cross-bitness paired IPC tests require the explicit Python gate. Do not
  # launch an arbitrary path or infer the other executable from PATH.
endif
