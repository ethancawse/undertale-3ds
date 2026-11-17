"""Synthetic package/extract/link tests; no game, saves, SDK installation or network."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import ssl
import urllib.error
import urllib.parse
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0,str(Path(__file__).resolve().parent/'builder'))
import cleanup
import library_pins as libraries
import toolchain


def native_test_bash(platform=sys.platform, env=None):
    env = os.environ if env is None else env
    override = env.get('BUILDER_TEST_BASH')
    found = toolchain.shutil.which('bash')
    if platform != 'win32':
        return override or found
    candidates = [override, found]
    candidates.extend(str(Path(root)/'msys2/usr/bin/bash.exe')
                      for root in toolchain.candidate_roots(env, 'win32'))
    for root in (env.get('ProgramFiles', 'C:/Program Files'),
                 env.get('ProgramFiles(x86)', 'C:/Program Files (x86)')):
        candidates.extend(str(Path(root)/'Git'/folder/'bash.exe') for folder in ('bin', 'usr/bin'))
    candidates.append('C:/msys64/usr/bin/bash.exe')
    for candidate in candidates:
        if not candidate:
            continue
        parts = str(candidate).replace('\\', '/').lower().split('/')
        if any(part in ('windowsapps', 'system32', 'syswow64', 'sysnative') for part in parts):
            continue  # WSL launchers cannot run this native Windows-path fixture.
        if Path(candidate).is_file():
            return str(candidate)
    return None


class LibraryPinsTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.sdk=self.root/'installed'
        (self.sdk/'cmake').mkdir(parents=True)
        (self.sdk/'cmake/3DS.cmake').write_text('# synthetic toolchain\n')
        (self.sdk/'libctru/include').mkdir(parents=True)
        (self.sdk/'libctru/include/personal.h').write_bytes(b'NEIGHBOUR HEADER')
        (self.sdk/'libctru/lib').mkdir()
        self.original=self.sdk/'libctru/lib/libctru.a'
        self.original.write_bytes(b'NEWER UNQUALIFIED LIBRARY')
        self.work=self.root/'work'
        self.bundle=self.root/'bundled'
        self.bundle.mkdir()
        self.pin=dict(package='libctru',version='2.7.0-1',extension='xz',prefix='libctru',library='libctru.a',
            library_sha256=hashlib.sha256(b'QUALIFIED LIBRARY').hexdigest())
        archive=self.bundle/libraries.filename(self.pin)
        with tarfile.open(archive,'w:xz') as tar:
            for relative,data in [('opt/devkitpro/libctru/lib/libctru.a',b'QUALIFIED LIBRARY'),
                                  ('opt/devkitpro/libctru/include/tested.h',b'QUALIFIED HEADER')]:
                info=tarfile.TarInfo(relative)
                info.size=len(data)
                tar.addfile(info,io.BytesIO(data))
        signature=Path(str(archive)+'.sig')
        signature.write_bytes(b'S'*566)
        self.pin.update(size=archive.stat().st_size,sha256=libraries.digest(archive),signature=libraries.digest(signature))
        self.tc=SimpleNamespace(root=str(self.sdk),bash='/bin/bash',platform=sys.platform,
            package_versions={'libctru':'2.7.1-1'})
        self.host=mock.patch.object(toolchain,'host_tool',return_value='cmake')
        self.host.start()
        self.addCleanup(self.host.stop)
        self.events=[]
        self.requests=[]
        stack=mock.patch.object(libraries,'require_stack',return_value={})
        stack.start()
        self.addCleanup(stack.stop)

    def extract(self,argv,cwd=None):
        self.events.append(list(map(str,argv)))
        with tarfile.open(argv[-1]) as tar:
            if argv[-2]=='tf':
                return '\n'.join(tar.getnames())+'\n'
            for info in tar.getmembers():
                dest=Path(cwd)/info.name
                dest.parent.mkdir(parents=True,exist_ok=True)
                with tar.extractfile(info) as stream:
                    dest.write_bytes(stream.read())
        return ''

    def fetch(self,request,timeout):
        self.requests.append(request.full_url)
        path=self.bundle/urllib.parse.urlsplit(request.full_url).path.rsplit('/',1)[-1]
        if not path.exists():
            raise urllib.error.HTTPError(request.full_url,404,'Not Found',{},None)
        return io.BytesIO(path.read_bytes())

    def select(self,**kwargs):
        return libraries.select(self.tc,self.work,packages=(self.pin,),opener=kwargs.pop('opener',self.fetch),
                                run=kwargs.pop('run',self.extract),**kwargs)

    def test_mismatch_downloads_verified_bytes_without_sdk_writes(self):
        before=libraries.inventory(self.sdk)
        messages=[]
        with mock.patch('urllib.request.urlopen',side_effect=AssertionError('no network')):
            state=self.select(notify=messages.append)
        self.assertEqual(before,libraries.inventory(self.sdk))
        self.assertEqual(libraries.digest(Path(state['sdk'])/'libctru/lib/libctru.a'),self.pin['library_sha256'])
        self.assertEqual((Path(state['sdk'])/'libctru/include/personal.h').read_bytes(),b'NEIGHBOUR HEADER')
        self.assertEqual(state['packages'][0]['effective'],'2.7.0-1')
        self.assertIn('from the work folder',' '.join(messages))
        self.assertEqual(len(self.requests),2)
        self.assertTrue((Path(state['shims'])/'cmake').is_file())

    def test_a_rerun_keeps_the_toolchain_file_path_and_a_changed_folder_is_never_reused(self):
        # 11-17 playtester: a new wrapper folder per run moved CMAKE_TOOLCHAIN_FILE, so every rerun printed CMake's
        # "You have changed variables that require your cache to be deleted".
        with mock.patch('urllib.request.urlopen',side_effect=AssertionError('no network')):
            first=self.select()['shims']
            self.assertEqual(self.select()['shims'],first)
            toolchain_file=Path(first)/'toolchain.cmake'
            toolchain_file.write_text(toolchain_file.read_text()+'# changed\n')
            second=self.select()['shims']
            self.assertEqual(self.select()['shims'],second)                   # the new folder is stable in turn
        self.assertNotEqual(second,first)
        self.assertTrue(toolchain_file.read_text().endswith('# changed\n'))     # left as found
        self.assertFalse((Path(second)/'toolchain.cmake').read_text().endswith('# changed\n'))

    def test_matching_install_does_not_open_or_unpack_bundles(self):
        self.original.write_bytes(b'QUALIFIED LIBRARY')
        self.tc.package_versions['libctru']='2.7.0-1'
        with mock.patch.object(libraries,'_unpack',side_effect=AssertionError('not needed')):
            state=self.select()
        self.assertEqual(state['sdk'],str(self.sdk))
        self.assertEqual(state['packages'][0]['source'],'installed')
        self.assertEqual(self.events,[])

    def test_windows_empty_path_matching_libraries_need_no_cmake_lookup(self):
        self.tc.platform='win32'
        self.tc.bash=str(self.sdk/'msys2/usr/bin/bash.exe')
        self.original.write_bytes(b'QUALIFIED LIBRARY')
        self.tc.package_versions['libctru']='2.7.0-1'
        with mock.patch.dict(os.environ,PATH=''), \
             mock.patch.object(toolchain,'host_tool',return_value=None) as host:
            state=self.select()
        host.assert_not_called()
        self.assertEqual(state['sdk'],str(self.sdk))
        self.assertEqual(self.requests,[])
        self.assertEqual(self.events,[])
        shim=(Path(state['shims'])/'cmake').read_text()
        self.assertIn(libraries.posix_path(Path(self.tc.bash).parent/'cmake.exe','win32'),shim)

    def test_windows_empty_path_mismatch_downloads_and_uses_selected_msys_cmake(self):
        self.tc.platform='win32'
        self.tc.bash=str(self.sdk/'msys2/usr/bin/bash.exe')
        with mock.patch.dict(os.environ,PATH=''), \
             mock.patch.object(toolchain,'host_tool',return_value=None) as host:
            state=self.select()
        host.assert_not_called()
        self.assertEqual(len(self.requests),2)
        self.assertEqual(state['packages'][0]['source'],'private')
        self.assertEqual([row[0] for row in self.events],
                         [str(Path(self.tc.bash).parent/'cmake.exe')]*2)

    def test_mac_private_cmake_setup_location_works_without_path_for_both_cases(self):
        self.host.stop()
        self.tc.platform='darwin'
        executable=self.work/'tools/bin/cmake/cmake-3.31.8-macos-universal/CMake.app/Contents/bin/cmake'
        executable.parent.mkdir(parents=True)
        executable.write_text('#!/bin/sh\nexit 0\n')
        executable.chmod(0o755)
        for matching in (True,False):
            with self.subTest(matching=matching):
                self.original.write_bytes(b'QUALIFIED LIBRARY' if matching else b'NEWER UNQUALIFIED LIBRARY')
                self.tc.package_versions['libctru']='2.7.0-1' if matching else '2.7.1-1'
                with mock.patch.dict(os.environ,PATH=''), mock.patch.object(toolchain.shutil,'which',return_value=None):
                    state=self.select()
                self.assertIn(str(executable),(Path(state['shims'])/'cmake').read_text())
                if not matching:self.assertEqual([row[0] for row in self.events],[str(executable)]*2)

    def test_same_version_with_wrong_bytes_still_selects_tested_copy(self):
        self.tc.package_versions['libctru']='2.7.0-1'
        state=self.select()
        self.assertEqual(state['packages'][0]['source'],'private')

    def test_missing_bundle_wrong_hash_and_wrong_signature_refuse_without_fallback(self):
        for which in ('missing','hash','signature'):
            with self.subTest(which=which):
                pin=copy.deepcopy(self.pin)
                if which=='missing':
                    pin['version']='absent'
                else:
                    pin['sha256' if which=='hash' else 'signature']='0'*64
                with self.assertRaises(toolchain.Refused) as caught:
                    libraries.select(self.tc,self.work,packages=(pin,),opener=self.fetch,run=self.extract)
                self.assertIn('Your installed devkitPro was not changed.',str(caught.exception))
                self.assertEqual(self.original.read_bytes(),b'NEWER UNQUALIFIED LIBRARY')

    def test_interrupted_extract_is_not_reused_and_retry_finishes(self):
        def interrupt(argv,cwd=None):
            if argv[-2]=='xf':
                (Path(cwd)/'partial').write_bytes(b'PART')
                raise OSError('interrupted extraction')
            return self.extract(argv,cwd)
        with self.assertRaises(toolchain.Refused):
            self.select(run=interrupt)
        self.assertEqual(list((self.work/'bin/libraries').glob('sdk-*/receipt.json')),[])
        self.assertEqual(self.select()['packages'][0]['source'],'private')

    def test_retry_verifies_and_reuses_cache_without_bundles_or_extraction(self):
        first=self.select()
        for path in self.bundle.iterdir():
            path.unlink()
        with mock.patch.object(libraries,'_unpack',side_effect=AssertionError('cached')):
            second=self.select()
        self.assertEqual(first['sdk'],second['sdk'])

    def test_changed_cache_is_preserved_and_a_new_verified_copy_is_used(self):
        first=self.select()
        old=Path(first['sdk'])/'libctru/lib/libctru.a'
        old.write_bytes(b'PERSONAL OR DAMAGED')
        second=self.select()
        self.assertNotEqual(first['sdk'],second['sdk'])
        self.assertEqual(old.read_bytes(),b'PERSONAL OR DAMAGED')

    def test_private_files_are_owned_and_reclaimable_but_existing_files_are_kept(self):
        (self.work/'bin').mkdir(parents=True)
        own=self.work/'bin/own.txt'
        own.write_bytes(b'PERSONAL')
        snapshot=cleanup.begin_build(self.work,[self.work/'bin'])
        state=self.select()
        cleanup.finish_build(self.work,snapshot)
        plan=cleanup.plan(self.work)
        expected=Path(state['sdk'])/'libctru/lib/libctru.a'
        self.assertIn(expected.resolve(),{Path(item.path).resolve() for item in plan.files})
        deleted, kept=cleanup.remove(plan,['build'])
        self.assertIn(str(expected),deleted)
        self.assertFalse(expected.exists())
        self.assertEqual(own.read_bytes(),b'PERSONAL')

    def test_link_hash_uses_configured_path_instead_of_assuming_the_sdk(self):
        state=self.select()
        build=self.root/'build'
        (build/'CMakeFiles/ctrport.dir').mkdir(parents=True)
        link=build/'CMakeFiles/ctrport.dir/link.txt'
        qualified=Path(state['sdk'])/'libctru/lib'
        link.write_text('gcc -L"'+qualified.as_posix()+'" -lctru -lm')
        row=libraries.actual_linked(build,sys.platform,(self.pin,))[0]
        self.assertEqual(row['sha256'],self.pin['library_sha256'])
        link.write_text('gcc -L"'+(self.sdk/'libctru/lib').as_posix()+'" -lctru -lm')
        with self.assertRaisesRegex(ValueError,'not the tested library'):
            libraries.actual_linked(build,sys.platform,(self.pin,))

    def test_package_input_pins_cover_signatures_as_well_as_archives(self):
        self.assertIsNone(libraries.verify_package(self.bundle,(self.pin,)))
        self.assertFalse(hasattr(libraries,'package_files'))
        signature=self.bundle/(libraries.filename(self.pin)+'.sig')
        signature.write_bytes(b'X'*566)
        with self.assertRaisesRegex(ValueError,'does not match its pin'):
            libraries.verify_package(self.bundle,(self.pin,))

    def configured(self):
        state=self.select()
        build=self.root/'build'
        (build/'CMakeFiles/ctrport.dir').mkdir(parents=True)
        archive=Path(state['sdk'])/'libctru/lib/libctru.a'
        (build/'CMakeFiles/ctrport.dir/link.txt').write_text('gcc -L"'+archive.parent.as_posix()+'" -lctru -lm')
        return build,archive

    def test_map_must_confirm_the_selected_path_even_when_other_bytes_match(self):
        build,archive=self.configured()
        linkmap=build/'release-link.map'
        linkmap.write_text(archive.as_posix()+'(synthetic.o)\nDiscarded input sections\n')
        self.assertEqual(libraries.confirm_linked(build,sys.platform,(self.pin,))[0]['sha256'],self.pin['library_sha256'])
        self.original.write_bytes(archive.read_bytes())
        linkmap.write_text(self.original.as_posix()+'(synthetic.o)\nDiscarded input sections\n')
        with self.assertRaisesRegex(ValueError,'did not use the checked'):
            libraries.confirm_linked(build,sys.platform,(self.pin,))
        linkmap.unlink()
        with self.assertRaisesRegex(ValueError,'did not use the checked'):
            libraries.confirm_linked(build,sys.platform,(self.pin,))

    def compiler_runtime(self):
        """A qualified compiler's layout in the temporary root: bin/arm-none-eabi-gcc and its private libgcc, whose
        bytes match no pin (so only the compiler-runtime rule can admit it)."""
        compiler=self.root/'devkitARM/bin'/('arm-none-eabi-gcc.exe' if sys.platform=='win32' else 'arm-none-eabi-gcc')
        compiler.parent.mkdir(parents=True)
        compiler.write_bytes(b'')
        runtime=self.root/'devkitARM/lib/gcc/arm-none-eabi/16.1.0/armv6k/fpu/libgcc.a'
        runtime.parent.mkdir(parents=True)
        runtime.write_bytes(b'HOST LIBGCC, NO PIN')
        return compiler,runtime

    def linked_with_runtime(self):
        """A configured build whose map takes one member from the compiler's runtime, and a function that writes its
        link command ({compiler} and {libs} filled in)."""
        build,archive=self.configured()
        compiler,runtime=self.compiler_runtime()
        (build/'release-link.map').write_text(
            archive.as_posix()+'(known.o)\n'+runtime.as_posix()+'(_udivsi3.o)\nDiscarded input sections\n')
        def link(command):
            (build/'CMakeFiles/ctrport.dir/link.txt').write_text(
                command.format(compiler=compiler.as_posix(),libs='-L"'+archive.parent.as_posix()+'" -lctru -lm -lgcc'))
        return build,runtime,link

    def test_plain_link_command_admits_its_compilers_runtime_as_before(self):
        # The S7 link: the compiler is the command's first argument.
        build,runtime,link=self.linked_with_runtime()
        link('"{compiler}" {libs}')
        rows=libraries.confirm_linked(build,sys.platform,(self.pin,))
        self.assertIn(str(runtime.resolve()),[row['path'] for row in rows])

    def test_launcher_wrapped_link_command_admits_the_compilers_runtime(self):
        # aacout's RULE_LAUNCH_LINK runs its link check first: python ... --aacout-link <out> -- <compiler> ...
        build,runtime,link=self.linked_with_runtime()
        link('"'+Path(sys.executable).as_posix()+'" -X utf8 -I "aacout_compile.py" --aacout-link "aacout.ld" -- "{compiler}" {libs}')
        rows=libraries.confirm_linked(build,sys.platform,(self.pin,))
        self.assertIn(str(runtime.resolve()),[row['path'] for row in rows])

    @unittest.skipUnless(sys.platform=='win32','a case-insensitive file system resolves the name to its real case')
    def test_plain_link_command_with_an_uppercase_compiler_path_admits_as_before(self):
        # The old check took the resolved name: an uppercase spelling of the real compiler resolves to its real name.
        build,runtime,link=self.linked_with_runtime()
        link('"'+(self.root/'devkitARM/bin/ARM-NONE-EABI-GCC.EXE').as_posix()+'" {libs}')
        rows=libraries.confirm_linked(build,sys.platform,(self.pin,))
        self.assertIn(str(runtime.resolve()),[row['path'] for row in rows])

    def test_compiler_runtime_is_refused_without_the_qualified_compiler(self):
        build,runtime,link=self.linked_with_runtime()
        python='"'+Path(sys.executable).as_posix()+'" -I "aacout_compile.py"'
        for command in (
                python+' -- gcc {libs}',                     # a launcher, but no qualified compiler after '--'
                python+' "{compiler}" -- gcc {libs}',        # the compiler's name only before the '--'
                python+' "{compiler}" {libs}',               # no '--': never a later argument, never args[0]
                'ccache "{compiler}" {libs}',                # a plain command must start with the compiler
                python+' -- gcc -o "{compiler}" {libs}',     # an output operand named like the compiler
                python+' -- gcc "{compiler}" {libs}',        # an input operand named like the compiler
                python+' -- ccache "{compiler}" {libs}',     # a non-compiler driver in the launcher's slot
                python+' -- -- "{compiler}" {libs}',         # a second separator: nothing past it
                python+' {libs} --'):                        # the launcher's slot is absent
            with self.subTest(command=command):
                link(command)
                with self.assertRaisesRegex(ValueError,'unpinned library'):
                    libraries.confirm_linked(build,sys.platform,(self.pin,))

    def test_same_length_damage_stops_the_configured_precompile_check(self):
        build,archive=self.configured()
        archive.write_bytes(b'X'*archive.stat().st_size)
        with self.assertRaisesRegex(ValueError,'not the tested library'):
            libraries.actual_linked(build,sys.platform,(self.pin,))

    def test_wrapper_guards_cached_roots_and_overrides_devkit_env_on_each_host(self):
        for platform in ('win32','linux','darwin'):
            with self.subTest(platform=platform):
                self.tc.platform=platform
                state=self.select()
                wrapper=(Path(state['shims'])/'toolchain.cmake').read_text()
                self.assertLess(wrapper.index('set(ENV{DEVKITPRO}'),wrapper.index('include('))
                self.assertIn('set(ENV{DEVKITARM}',wrapper)
                self.assertIn(str(self.sdk).replace('\\','/').split(':',1)[-1],wrapper)
                shim=(Path(state['shims'])/'cmake').read_text()
                self.assertNotIn('--fresh',shim)
                self.assertIn('-E rm -rf --',shim)
                self.assertIn('.builder-library-root',shim)
                self.assertLess(shim.index('--check-links'),shim.index('"${args[@]}" || exit'))

    def test_real_cmake_reconfigures_changed_sdk_without_fresh_and_keeps_other_files(self):
        cmake=os.environ.get('BUILDER_TEST_CMAKE') or toolchain.shutil.which('cmake')
        bash=native_test_bash()
        if not cmake:self.skipTest('Real host CMake is needed; set BUILDER_TEST_CMAKE')
        if not bash:self.skipTest('Native Git or MSYS bash is needed on Windows; WSL bash cannot run this test'
                                  if sys.platform == 'win32' else 'Real host bash is needed')
        project=self.root/'project'
        project.mkdir()
        (project/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.19)\nproject(CacheProbe NONE)\n')
        build=self.root/'build with spaces'
        build.mkdir()
        personal=build/'personal.txt'
        personal.write_text('Keep this file')
        self.tc.platform='linux'  # Exercise the portable shell with an explicit host executable.
        def wrapper(sdk, name):
            return libraries.write_wrapper(self.work/'bin/libraries'/name,self.tc,sdk,Path(cmake).as_posix())
        def configure(folder,*extra,success=True):
            result=subprocess.run([bash,str(Path(folder)/'cmake'),'-S',project.as_posix(),'-B',build.as_posix(),*extra],
                stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30,
                **toolchain.hidden_options())
            self.assertEqual(result.returncode==0,success,result.stdout)
        first=wrapper(self.sdk,'run-first')
        configure(first,'-DR366_CACHE=first')
        marker=(build/'.builder-library-root').read_text()
        sentinel=build/'CMakeFiles/stale-root'
        sentinel.write_text('cached root')
        configure(first)
        self.assertIn('R366_CACHE:UNINITIALIZED=first',(build/'CMakeCache.txt').read_text())
        self.assertTrue(sentinel.exists())
        second=wrapper(self.root/'private-sdk','run-second')
        configure(second,'-G','No such generator',success=False)
        self.assertEqual((build/'.builder-library-root').read_text(),marker)
        configure(second)
        self.assertNotIn('R366_CACHE:',(build/'CMakeCache.txt').read_text())
        self.assertFalse(sentinel.exists())
        self.assertNotEqual((build/'.builder-library-root').read_text(),marker)
        self.assertEqual(personal.read_text(),'Keep this file')
        configure(first)
        self.assertEqual((build/'.builder-library-root').read_text(),marker)

    def test_windows_native_bash_ignores_wsl_with_git_or_registered_msys_available(self):
        sdk='D:/Registered SDK'
        git='C:/Program Files/Git/bin/bash.exe'
        msys=sdk+'/msys2/usr/bin/bash.exe'
        stubs=('C:/Users/fixture/AppData/Local/Microsoft/WindowsApps/bash.EXE',
               'C:/Windows/System32/bash.exe')
        for stub in stubs:
            for native in (git,msys):
                with self.subTest(stub=stub,native=native), \
                     mock.patch.object(toolchain.shutil,'which',return_value=stub), \
                     mock.patch.object(toolchain,'candidate_roots',return_value=[sdk]), \
                     mock.patch.object(Path,'is_file',lambda p: str(p).replace('\\','/').lower()
                         in {stub.lower(),native.lower()}):
                    selected=native_test_bash('win32',{'BUILDER_TEST_BASH':stub})
                    self.assertEqual(str(selected).replace('\\','/'),native)

    def test_windows_without_native_bash_returns_none_and_real_probe_skips_plainly(self):
        stub='C:/Windows/System32/bash.exe'
        with mock.patch.object(toolchain.shutil,'which',return_value=stub), \
             mock.patch.object(toolchain,'candidate_roots',return_value=[]), \
             mock.patch.object(Path,'is_file',lambda p: str(p).replace('\\','/').lower()==stub.lower()):
            self.assertIsNone(native_test_bash('win32',{}))
        with mock.patch.dict(os.environ,BUILDER_TEST_CMAKE='synthetic cmake'), \
             mock.patch.object(sys.modules[__name__],'native_test_bash',return_value=None):
            with self.assertRaisesRegex(unittest.SkipTest,'bash'):
                self.test_real_cmake_reconfigures_changed_sdk_without_fresh_and_keeps_other_files()

    def test_unix_bash_keeps_the_host_lookup_and_explicit_override(self):
        with mock.patch.object(toolchain.shutil,'which',return_value='/bin/bash'), \
             mock.patch.object(toolchain,'candidate_roots',side_effect=AssertionError('Windows-only lookup')):
            for host in ('linux','darwin'):
                self.assertEqual(native_test_bash(host,{}),'/bin/bash')
                self.assertEqual(native_test_bash(host,{'BUILDER_TEST_BASH':'/private/bash'}),'/private/bash')

    def test_no_package_inputs_are_accepted_or_shipped_by_packaging(self):
        sys.path.insert(0,str(Path(__file__).resolve().parent))
        import package_builder
        import inspect
        import zipfile
        self.assertNotIn('library_packages',inspect.signature(package_builder.package).parameters)
        source=self.root/'export'
        contract=(Path(package_builder.__file__).resolve().parents[2]/'docs/release/build-contract.json').read_text()
        for relative in package_builder.pipeline.SOURCE_FILES+package_builder.EXTRA_FILES:
            path=source/relative
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text('0.1.0\n' if relative=='VERSION' else
                contract if relative=='docs/release/build-contract.json' else 'fixture')
        for relative in package_builder.DIRECTORIES:(source/relative).mkdir(parents=True,exist_ok=True)
        (source/'tools/release/builder/builder.py').write_text('FIXED_GAME = None\n')
        stray=source/'tools/release/builder/bin/libraries'
        stray.mkdir(parents=True)
        for path in self.bundle.iterdir():(stray/path.name).write_bytes(path.read_bytes())
        (stray/'libctru.a').write_bytes(b'SYNTHETIC LIBRARY')
        for platform in ('windows','linux','macos'):
            with self.subTest(platform=platform), mock.patch.object(package_builder.runtime_pins,'windows_files',
                    return_value={'python.exe':b'synthetic runtime'}):
                archive=package_builder.package(source,self.root/'packages','undertale',platform,
                    self.root/'runtime' if platform=='windows' else None)
                if platform=='windows':
                    with zipfile.ZipFile(archive) as package:names=package.namelist()
                else:
                    with tarfile.open(archive) as package:names=package.getnames()
                self.assertTrue(any(name.endswith('/tools/release/builder/builder.py') for name in names))
                self.assertFalse(any('/libraries/' in name or name.endswith(('.pkg.tar.zst','.pkg.tar.xz','.sig','.a'))
                    for name in names),names)

    def test_release_day_driver_does_not_offer_package_inputs(self):
        sys.path.insert(0,str(Path(__file__).resolve().parent))
        import package_builder
        if not Path(package_builder.__file__).with_name('undertale_release_day.py').is_file():
            self.skipTest('Release-day driver is not part of the public export')
        import undertale_release_day as day
        args=day.package_arguments('linux',bound=Path('source'),game='undertale',kits=Path('out'),
            runtime=Path('runtime'),mac_runtimes={},mac_licences=Path('licences'))
        self.assertNotIn('--library-packages',args)

    def test_download_failure_classes_are_distinct_and_leave_no_trusted_partial(self):
        cases=[('missing',urllib.error.HTTPError('https://fixture',404,'missing',{},None),'could not be found'),
               ('network',urllib.error.URLError('no network'),'Check your connection'),
               ('certificate',urllib.error.URLError(ssl.SSLCertVerificationError(1,'untrusted')),'certificate')]
        before=libraries.inventory(self.sdk)
        for name,error,words in cases:
            with self.subTest(name=name):
                def fail(*a,**kw):raise error
                with self.assertRaisesRegex(toolchain.Refused,words):self.select(opener=fail)
                self.assertEqual(before,libraries.inventory(self.sdk))
                self.assertFalse(list(self.work.rglob('*.part')))
                self.assertFalse(list(self.work.rglob('receipt.json')))
                self.assertFalse(list((self.work/'bin/libraries/downloads').glob('pkg-*')))

    def test_partial_response_and_hash_mismatch_do_not_install_or_retry(self):
        before=libraries.inventory(self.sdk)
        for contents in (b'cut short',b'X'*self.pin['size'],b'X'*(self.pin['size']+1)):
            with self.subTest(size=len(contents)):
                calls=[]
                def wrong(*a,**kw):calls.append(a);return io.BytesIO(contents)
                with self.assertRaisesRegex(toolchain.Refused,'did not match the required file'):self.select(opener=wrong)
                self.assertEqual(len(calls),1)
                self.assertEqual(before,libraries.inventory(self.sdk))
                self.assertFalse(list(self.work.rglob('*.part')))
                self.assertFalse(list(self.work.rglob('receipt.json')))

    def test_offline_start_uses_rechecked_archive_and_extracted_cache(self):
        first=self.select()
        def offline(*a,**kw):raise AssertionError('must not access network')
        second=self.select(opener=offline)
        self.assertEqual(first['sdk'],second['sdk'])
        self.assertEqual(len(self.requests),2)

    def test_corrupt_download_cache_is_rechecked_and_refetched(self):
        first=self.select()
        archive=next((self.work/'bin/libraries/downloads').glob('pkg-*/*'+self.pin['extension']))
        archive.write_bytes(b'X'*archive.stat().st_size)
        second=self.select()
        self.assertEqual(len(self.requests),4)
        self.assertEqual(first['sdk'],second['sdk'])
        self.assertEqual(self.original.read_bytes(),b'NEWER UNQUALIFIED LIBRARY')

    def test_final_map_refuses_an_extra_unpinned_library(self):
        build,archive=self.configured()
        extra=self.sdk/'libctru/lib/libcitro2d.a'
        extra.write_bytes(b'NOT IN THE TABLE')
        (build/'release-link.map').write_text(archive.as_posix()+'(known.o)\n'+extra.as_posix()+'(extra.o)\n')
        with self.assertRaisesRegex(ValueError,'unpinned library'):libraries.confirm_linked(build,sys.platform,(self.pin,))

    def test_precompile_refuses_an_extra_unpinned_library(self):
        build,archive=self.configured()
        link=build/'CMakeFiles/ctrport.dir/link.txt'
        link.write_text(link.read_text()+' -lcitro2d')
        with self.assertRaisesRegex(ValueError,'unpinned library'):libraries.actual_linked(build,sys.platform,(self.pin,))

    def test_build_refuses_a_failed_map_check_before_replacing_any_output(self):
        sys.path.insert(0,str(Path(__file__).resolve().parent))
        import pipeline
        from test_builder import BuildTest
        fixture=BuildTest('test_other_exit_two_failures_keep_the_build_retry')
        fixture.setUp()
        try:
            job,seen=fixture.make(['[100%] Built target ctrport\n'])
            with mock.patch.object(pipeline.library_pins,'select',return_value={
                    'sdk':str(self.sdk),'packages':[],'shims':None}), \
                 mock.patch.object(pipeline.library_pins,'confirm_linked',side_effect=ValueError('wrong linked root')) as check, \
                 mock.patch.object(job,'collect') as collect:
                with self.assertRaisesRegex(pipeline.BuildFailed,'wrong linked root'):
                    job.run()
                check.assert_called_once()
                collect.assert_not_called()
                self.assertIn('argv',seen)
        finally:
            fixture.doCleanups()
            fixture.tearDown()


    def test_no_resolved_root_refuses_without_reading_relative_or_default_libraries(self):
        self.tc.root=None
        with mock.patch.object(libraries,'library_path',side_effect=AssertionError('no SDK library read')), \
             mock.patch.object(libraries,'command',side_effect=AssertionError('no compiler or extract')):
            with self.assertRaisesRegex(toolchain.Refused,"3DS compiler wasn't found"):
                self.select()
        self.assertFalse(self.work.exists())

    def test_registered_bash_without_a_compiler_refuses_before_library_selection(self):
        # These are synthetic installer roots. A default installation is not a fallback
        # once detection has selected the registered location.
        registered=self.root/'registered SDK'
        bash=registered/'msys2/usr/bin/bash.exe'
        bash.parent.mkdir(parents=True)
        bash.write_bytes(b'synthetic bash')
        with mock.patch.object(toolchain,'windows_install_roots',return_value=[str(registered)]):
            tc=toolchain.detect(str(self.root/'tools'),env={},platform='win32',
                run=lambda argv:(0,'\n'.join(p+' 1.0-1' for p in toolchain.REQUIRED_PACKAGES)))
        self.assertEqual(tc.root,str(registered))
        self.assertFalse(tc.can_build)
        with mock.patch.object(libraries,'library_path',side_effect=AssertionError('no fallback SDK read')):
            with self.assertRaisesRegex(toolchain.Refused,"3DS compiler wasn't found"):
                libraries.select(tc,self.work)
        self.assertFalse(self.work.exists())


class UnpackToolFailureTests(LibraryPinsTests):
    """N13.2: a CMake that can't start (msys-jsoncpp-27.dll missing) is named with its exit code, never the generic
    unpack text, and the command and output reach the build log through Refused.details."""
    def test_unpack_tool_that_cannot_start_is_named_with_its_code(self):
        def broken(argv, cwd=None):
            return libraries.command(argv, cwd, run=lambda argv, timeout, cwd: (0xC0000135, ''))
        with self.assertRaises(toolchain.Refused) as caught:
            self.select(run=broken)
        text = str(caught.exception)
        self.assertTrue(text.startswith(libraries.REFUSAL + '\n'), text)
        self.assertNotIn('The library archive could not be unpacked.', text)
        if sys.platform == 'win32':
            self.assertIn("can't start: a file it needs is missing (exit code 0xC0000135).", text)
            self.assertIn('Click Update devkitPro on the compiler page', text)
        self.assertIn('Command: ', caught.exception.details)
        self.assertIn(' -E tar tf ', caught.exception.details)


for _name in [n for n in vars(LibraryPinsTests) if n.startswith('test_')]:
    setattr(UnpackToolFailureTests, _name, None)   # run only the test above with the shared fixture


class StackAndTlsTests(unittest.TestCase):
    def test_package_and_rule_changes_refuse_before_a_download_or_work_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            rule=root/'cmake/3DS.cmake'
            rule.parent.mkdir()
            rule.write_bytes(b'TESTED RULE')
            pins={'packages':{'devkitarm-binutils':'2.46.0-1','devkitarm-newlib':'4.6.0.20260123-5',
                              'devkitarm-rules':'1.6.0-4','3ds-cmake':'1.5.2-1'},
                  'files':{'cmake/3DS.cmake':libraries.digest(rule)},'runtime_archives':{}}
            tc=SimpleNamespace(root=str(root),package_versions=dict(pins['packages']))
            self.assertEqual(libraries.require_stack(tc,pins)['packages'],pins['packages'])
            for name in pins['packages']:
                with self.subTest(package=name):
                    tc.package_versions[name]='999.0-1'
                    with self.assertRaisesRegex(toolchain.Refused,name):libraries.require_stack(tc,pins)
                    tc.package_versions[name]=pins['packages'][name]
            rule.write_bytes(b'EDITED RULE')
            with self.assertRaisesRegex(toolchain.Refused,'rules or compiler libraries'):
                libraries.require_stack(tc,pins)

    def test_stack_queries_fresh_versions_on_each_host_and_refuses_a_failed_query(self):
        pins=json.loads((Path(libraries.__file__).with_name('sdk-stack-pins.json')).read_text())
        output='\n'.join(k+' '+v for k,v in pins['packages'].items())
        for host in ('win32','linux','darwin'):
            with self.subTest(host=host):
                tc=SimpleNamespace(root='/synthetic/sdk',platform=host,bash='/synthetic/bash',package_versions={})
                with mock.patch.object(toolchain,'run_capture',return_value=(0,output)) as query, \
                     mock.patch.object(libraries.Path,'is_file',return_value=True), \
                     mock.patch.object(libraries,'digest',side_effect=lambda p: dict(pins['files'],**pins['runtime_archives'])[str(p).replace('\\','/').split('/synthetic/sdk/',1)[1]]):
                    libraries.require_stack(tc)
                    self.assertIn('-Q',query.call_args.args[0][-1] if host=='win32' else query.call_args.args[0])
                    self.assertEqual(query.call_args.kwargs['timeout'],30)
                with mock.patch.object(toolchain,'run_capture',return_value=(1,'unreadable')):
                    with self.assertRaisesRegex(toolchain.Refused,'Could not check'):libraries.require_stack(tc)

    def test_slow_stack_query_timeout_is_not_reported_as_a_wrong_version(self):
        for host in ('win32','linux','darwin'):
            with self.subTest(host=host):
                tc=SimpleNamespace(root='/synthetic/sdk',platform=host,bash='/synthetic/bash')
                with mock.patch.object(toolchain,'run_capture',side_effect=subprocess.TimeoutExpired('query',30)) as query:
                    with self.assertRaises(toolchain.Refused) as caught:libraries.require_stack(tc)
                self.assertEqual(query.call_args.kwargs['timeout'],30)
                message=str(caught.exception)
                self.assertIn('timed out',message)
                self.assertIn('Click Check again',message.replace('click Check again','Click Check again'))
                self.assertNotIn('found',message)
                self.assertNotIn('newer builder',message)

    def test_private_context_has_verified_roots_without_the_system_store(self):
        # Disable only this context's store import, never edit the OS store.
        with mock.patch.object(ssl.SSLContext,'load_default_certs') as defaults:
            context=toolchain.https_context()
        defaults.assert_called_once()
        self.assertTrue(context.check_hostname)
        self.assertEqual(context.verify_mode,ssl.CERT_REQUIRED)
        self.assertGreater(len(context.get_ca_certs()),100)

    def test_real_download_certificate_error_has_its_own_message_and_cleans_partial(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'download'
            def failed(*a,**kw):
                raise urllib.error.URLError(ssl.SSLCertVerificationError(1,'untrusted'))
            with self.assertRaisesRegex(toolchain.Refused,'server certificate'):
                toolchain.download('https://example.invalid/file',str(path),'0'*64,opener=failed)
            self.assertFalse(path.exists())
            self.assertFalse(Path(str(path)+'.part').exists())

    def test_https_request_uses_the_private_context(self):
        with mock.patch.object(urllib.request,'urlopen') as opened:
            toolchain.https_open('https://example.invalid/package',timeout=7)
        self.assertEqual(opened.call_args.kwargs['timeout'],7)
        self.assertIsInstance(opened.call_args.kwargs['context'],ssl.SSLContext)

    def test_source_gate_precedes_any_library_download(self):
        import pipeline
        from test_builder import BuildTest
        fixture=BuildTest('test_other_exit_two_failures_keep_the_build_retry')
        fixture.setUp()
        try:
            job,_=fixture.make([])
            with mock.patch.object(pipeline,'release_source',side_effect=pipeline.BuildFailed('source gate')), \
                 mock.patch.object(libraries,'select',side_effect=AssertionError('must not download')):
                with self.assertRaisesRegex(pipeline.BuildFailed,'source gate'):job.run()
        finally:
            fixture.doCleanups()
            fixture.tearDown()


if __name__=='__main__':
    unittest.main()
