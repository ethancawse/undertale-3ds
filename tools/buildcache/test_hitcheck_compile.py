"""Fail-closed routing matters: cover all readers and preserve the pair ABI."""
import importlib.util
import fnmatch
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

spec=importlib.util.spec_from_file_location('hitcheck',Path(__file__).with_name('hitcheck_compile.py'))
adapter=importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class RoutingTests(unittest.TestCase):
    def test_driver_emits_and_assembles_with_python39_path_writes(self):
        before=''
        for name in adapter.GM14_SLOTS:
            before+=' .type '+name+', %function\n'+name+':\n bx lr\n .size '+name+', .-'+name+'\n'
        expected,_=adapter.rewrite(before,'collision_gm14.c')
        write_text=Path.write_text
        def python39_write(path,text,**kwargs):
            if 'newline' in kwargs:
                raise TypeError('Python 3.9 Path.write_text has no newline keyword')
            return write_text(path,text,**kwargs)
        for mode in ('-S','-c'):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory() as folder:
                output=Path(folder)/('output.s' if mode=='-S' else 'output.o')
                commands=[]
                def compiler(command):
                    commands.append(command)
                    target=Path(command[command.index('-o')+1])
                    if '-S' in command:
                        with target.open('w',encoding='utf-8',newline='\n') as stream:
                            stream.write(before)
                    else:
                        self.assertIn('-Wa,--fatal-warnings',command)
                        self.assertEqual(Path(command[command.index('assembler')+2]).read_text(encoding='utf-8'),expected)
                        target.write_bytes(b'assembled fixture')
                    return 0
                with mock.patch.object(adapter,'run',side_effect=compiler),mock.patch.object(Path,'write_text',python39_write):
                    self.assertEqual(adapter.main([adapter.MARKER,'fixture-gcc','-DENABLE_WAD16',mode,
                                                   'collision_gm14.c','-o',str(output)]),0)
                self.assertEqual((output.parent/'hcm1.s').read_bytes(),expected.encode('utf-8'))
                self.assertEqual(output.read_bytes(),expected.encode('utf-8') if mode=='-S' else b'assembled fixture')
                self.assertEqual(len(commands),1 if mode=='-S' else 2)

    def test_vm_preserves_original_region_calls_and_registration_literals(self):
        before=''
        for name in ('Collision_computeBBox','Collision_instancesOverlapPrecise.isra.0'):
            before+=' .type '+name+', %function\n'+name+':\n bx lr\n'
        for caller in sorted(set(adapter.VM_BBOX)|set(adapter.VM_PAIR)|set(adapter.VM_UNCHANGED)):
            before+=' .type '+caller+', %function\n'+caller+':\n'
            before+=' bl Collision_computeBBox(PLT)\n'*(adapter.VM_BBOX.get(caller,0)+adapter.VM_UNCHANGED.get(caller,0))
            before+=' bl Collision_instancesOverlapPrecise.isra.0(PLT)\n'*adapter.VM_PAIR.get(caller,0)
            before+=' bx lr\n'
        literals=''.join(' .word '+name+'\n' for name in adapter.VM_UNCHANGED)*2
        registration=' .type VMBuiltins_registerAll, %function\nVMBuiltins_registerAll:\n bx lr\n'+literals+' .size VMBuiltins_registerAll, .-VMBuiltins_registerAll\n'
        after,receipt=adapter.rewrite(before+registration,'vm_builtins.c')
        self.assertIn('Hitcheck_retainVMPair:\n b Collision_instancesOverlapPrecise.isra.0\n',after)
        self.assertNotIn('Hitcheck_retainVMBBox',after)
        self.assertNotIn('Hitcheck_activateRegion',after)
        self.assertNotIn('Hitcheck_deactivateRegion',after)
        self.assertIn(registration,after)
        self.assertEqual(after.count('bl Collision_computeBBox'),2)
        self.assertEqual(receipt['unchanged_calls'],{'Collision_computeBBox':adapter.VM_UNCHANGED})
        for name in adapter.VM_UNCHANGED:
            self.assertIn(name+':\n bl Collision_computeBBox(PLT)\n bx lr\n',after)
        with self.assertRaises(AssertionError):
            adapter.rewrite(before.replace('builtin_instance_activate_region','unexpected_region')+registration,'vm_builtins.c')

    def test_gm14_slots_require_both_unique_bodies(self):
        before=''
        for name in adapter.GM14_SLOTS:
            before+=' .type '+name+', %function\n'+name+':\n bx lr\n .size '+name+', .-'+name+'\n'
        after,receipt=adapter.rewrite(before,'collision_gm14.c')
        for name,(capacity,end) in adapter.GM14_SLOTS.items():
            self.assertIn(end+':\n .space '+str(capacity)+'-(.-'+name+'), 0\n',after)
        for bad in ['',before+before,before.replace('Colflip14_pointHits','other')]:
            with self.assertRaises(AssertionError):adapter.rewrite(bad,'collision_gm14.c')

    def test_dlgtop_exclusion_rewrite_still_matches(self):
        root=Path(__file__).resolve().parents[2]
        layout=(root/'platform/3ds/late_reserve.ld').read_text(encoding='utf-8')
        # CMake's pre-existing replacement must still exclude main's appended
        # loading code. A new first exclusion previously defeated that match.
        # S8b respin: CMake now finds and replaces the bare '*z_utlife.o' (FATAL_ERROR when absent), since
        # dimlight's exclusions come first in the catch-all.
        self.assertEqual(layout.count('*z_utlife.o'),1)
        routed=layout.replace('*z_utlife.o','*source/main.o *z_utlife.o')
        self.assertIn('*source/main.o *z_utlife.o *z_hitcheck.o',routed)

    def fixture(self):
        result='header\n .type Collision_instancesOverlapPrecise.isra.0, %function\nCollision_instancesOverlapPrecise.isra.0:\n bx lr\n'
        calls={'dispatchCollisionEvents': [('Collision_computeBBox',4),('Collision_instancesOverlapPrecise.isra.0',2)],
               'stepFrame': [('Collision_computeBBox',1)], 'Colexact_sharedBBox': [('Collision_computeBBox',1)]}
        for caller,targets in calls.items():
            result+=' .type '+caller+', %function\n'+caller+':\n.Llocal:\n'
            for target,count in targets:
                result+=(' bl '+target+'(PLT)\n')*count
            result+=' bx lr\n'
        return result+' .type Runner_free, %function\nRunner_free:\n bl free\n pop {r4, lr}\n b free\n .size Runner_free, .-Runner_free\nfooter\n'

    def test_only_qualified_reader_call_operands_change(self):
        before=self.fixture();after,receipt=adapter.rewrite(before,'runner.c')
        restored=after.replace('Hitcheck_collisionBBox','Collision_computeBBox').replace('Hitcheck_pair','Collision_instancesOverlapPrecise.isra.0')
        restored=restored.replace('Hitcheck_freeRunner','free')
        self.assertEqual(before,restored[:len(before)])
        self.assertIn('Hitcheck_retainRunnerPair:\n b Collision_instancesOverlapPrecise.isra.0\n',after)
        self.assertEqual(after.count('bl Collision_computeBBox(PLT)'),1)
        self.assertEqual(receipt['unchanged_calls']['Collision_computeBBox'],{'Colexact_sharedBBox':1})

    def test_every_public_reader_is_qualified(self):
        self.assertEqual(adapter.VM_READERS, {
            'builtin_distance_to_point':1, 'builtin_distance_to_object':2,
            'VMBuiltins_getVariable':4, 'cdBoxSide':1})
        self.assertEqual(adapter.RUNNER_BBOX, {'dispatchCollisionEvents':4,'stepFrame':1})
        self.assertEqual({name:slot[0] for name,slot in adapter.READER_SLOTS['z_utflames.c'].items()},
                         {'utfRect':2080, 'utfBox':1612, 'utfCircle':1408, 'utfRectFast.constprop.0':2244})
        self.assertEqual({name:slot[0] for name,slot in adapter.READER_SLOTS['colline_gm14.c'].items()},
                         {'colline14FirstHit':472, 'builtin_collision_circle_gm14':244, 'Colline14_distanceToPoint':168})
        for source,slots in adapter.READER_SLOTS.items():
            before=''.join(' .type '+name+', %function\n'+name+':\n bx lr\n .size '+name+', .-'+name+'\n'
                           for name in slots)
            after,proof=adapter.rewrite(before,source)
            self.assertEqual(proof['slots'],slots)
            for name,(capacity,marker) in slots.items():
                self.assertIn(marker+':\n .space '+str(capacity)+'-(.-'+name+'), 0\n',after)
            with self.assertRaises(AssertionError):adapter.rewrite(before+before,source)

    def test_new_colline_helpers_escape_the_early_object_catchall(self):
        root=Path(__file__).resolve().parents[2]
        layout=(root/'platform/3ds/late_reserve.ld').read_text(encoding='utf-8')
        section='.text_hitcheck_readers_late'
        self.assertFalse(any(fnmatch.fnmatchcase(section,pattern) for pattern in ('.text','.text.*')))
        selector='KEEP(*('+section+'))'
        self.assertEqual(layout.count(selector),1)
        self.assertGreater(layout.index(selector),layout.index('*colline_gm14.o(.text .text.*)'))
        self.assertGreater(layout.index(selector),layout.index('*z_hitcheck.o(.text .text.* .rodata .rodata.*)'))

    def test_terminal_free_requires_unique_epilogue_branch(self):
        before=self.fixture()
        routed=adapter.route_runner_free(before)
        self.assertEqual(routed.replace('Hitcheck_freeRunner','free'),before)
        self.assertIn(' bl free\n pop {r4, lr}\n b Hitcheck_freeRunner',routed)
        for bad in [before.replace(' b free\n',' bl free\n'),
                    before.replace(' b free\n',' b free\n b free\n'),
                    before.replace(' pop {r4, lr}',' pop {r4, pc}'),
                    before.replace(' pop {r4, lr}',' pop {r4, lr}\n mov r0, r1')]:
            with self.assertRaises(AssertionError):adapter.route_runner_free(bad)

    def test_missing_extra_or_unrecognized_caller_refused(self):
        before=self.fixture()
        for variant in [before.replace(' bl Collision_computeBBox(PLT)\n','',1),
                        before.replace(' bx lr\n',' bl Collision_computeBBox(PLT)\n bx lr\n',1),
                        before.replace('dispatchCollisionEvents','drawing')]:
            with self.assertRaises(AssertionError):adapter.rewrite(variant,'runner.c')

    def test_retention_input_is_registered_for_layerfix(self):
        after,_=adapter.rewrite(self.fixture(),'runner.c')
        section=re.search(r'\.pushsection ([^,]+),',after)[1]
        root=Path(__file__).resolve().parents[2]
        layout=(root/'platform/3ds/late_reserve.ld').read_text(encoding='utf-8')
        # This is the input-name grammar enforced by check_layerfix_inputs.cmake.
        registered=re.findall(r'\.text\.late\.[A-Za-z0-9_]+',layout)
        self.assertIn(section,registered)
        self.assertIn('KEEP(*('+section+'))',layout)

    def test_pair_abi_and_each_parameter_are_checked(self):
        params=['struct DataWin * ISRA.1','bool ISRA.2','struct Instance * a','struct Instance * b']
        params+=['GMLReal ISRA.'+str(i) for i in range(8)]
        dump='bool Collision_instancesOverlapPrecise.isra ('+', '.join(params)+')\n'
        adapter.qualify_pair_abi(dump)
        adapter.qualify_pair_abi(dump.replace('bool', '_Bool'))
        for bad in [dump.replace('bool ISRA.2','int ISRA.2'),dump.replace('GMLReal ISRA.7','float ISRA.7'),dump+dump,'']:
            with self.assertRaises(AssertionError):adapter.qualify_pair_abi(bad)


if __name__=='__main__':unittest.main()
