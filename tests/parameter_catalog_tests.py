"""Offline extraction contracts and checked-in catalog provenance checks."""
import ast
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import generate_parameter_catalog as generator


class CatalogTests(unittest.TestCase):
    def test_union_tensor_and_documented_or_do_not_erase_scalar_types(self):
        self.assertEqual(generator.annotation_schema('float | Tensor'),
                         {'anyOf':[{'type':'number'},{'type':'external'}]})
        self.assertEqual(generator.annotation_schema('int or list[int]'),
                         {'anyOf':[{'type':'integer'},{'type':'array','items':{'type':'integer'}}]})
        self.assertEqual(generator.annotation_schema('Literal["cosine", "linear"]'), {'enum':['cosine','linear']})
        required=generator.field_record('rank','int',ast.parse('Field(..., ge=1)',mode='eval').body,
            'Required rank',{'provider':'test'},1)
        self.assertTrue(required['required'])
        self.assertNotIn('default',required)
        self.assertNotIn('default_expression',required)

    def test_multiline_cpp_initializers_preserve_sampler_order(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='catalog-') as directory:
            path=Path(directory)/'fixture.h'
            path.write_text('''struct common_params_sampling {
    float temp = 0.7f;
    std::vector<int> samplers = {
        3, 1, 2,
    };
    bool enabled = true;
};''')
            source={'provider':'llama','path':'fixture.h'}
            group=generator.cpp_groups(source,path)[0]
            self.assertEqual([f['name'] for f in group['parameters']],['temp','samplers','enabled'])
            self.assertEqual(group['parameters'][1]['default'],[3,1,2])

    def test_typescript_literals_nested_objects_and_optional_properties(self):
        schema=generator.ts_schema('number | false',{})
        self.assertEqual(schema,{'anyOf':[{'type':'number'},{'enum':[False]}]})
        nested=generator.ts_schema('{ type: "json"; schema?: Record<string, unknown>; }',{})
        self.assertEqual(nested['required'],['type'])
        self.assertEqual(nested['properties']['type'],{'enum':['json']})
        self.assertFalse(nested['additionalProperties'])

    def test_every_published_field_has_locked_provenance(self):
        catalog=json.loads((ROOT/'catalog/parameters.json').read_text())
        lock=json.loads((ROOT/'catalog/parameter-sources.json').read_text())
        sources={(s['provider'],s['path']):s for s in lock}
        ids=set()
        for group in catalog['groups']:
            self.assertNotIn(group['id'],ids); ids.add(group['id'])
            names=set()
            for field in group['parameters']:
                self.assertNotIn(field['name'],names); names.add(field['name'])
                for key in ('native_type','description','schema','source'):
                    self.assertTrue(field[key],(group['id'],field['name'],key))
                evidence=field['source']
                self.assertGreater(evidence['line'],0)
                if evidence['provider']=='iiLocalLLM':
                    self.assertEqual(evidence['sha256'],hashlib.sha256((ROOT/evidence['path']).read_bytes()).hexdigest())
                else:
                    source=sources[(evidence['provider'],evidence['path'])]
                    for key in ('sha256','revision','url'): self.assertEqual(evidence[key],source[key])
        for group in ('trl.DPOConfig','trl.GRPOConfig','peft.LoraConfig','torch.AdamW',
                      'mlx.Muon','sglang.ServerArgs','deepspeed.DeepSpeedZeroConfig',
                      'transformers.GenerationConfig','iiLocalLLM.GenerationOptions'):
            self.assertIn(group,ids)

    def test_coverage_counts_and_executable_binding_names(self):
        catalog=json.loads((ROOT/'catalog/parameters.json').read_text())
        report=json.loads((ROOT/'catalog/parameter-coverage.json').read_text())
        self.assertEqual(report['groups'],len(catalog['groups']))
        self.assertEqual(report['fields_including_inheritance'],sum(len(g['parameters']) for g in catalog['groups']))
        native=next(g for g in catalog['groups'] if g['id']=='iiLocalLLM.GenerationOptions')
        native_fields={f['name'] for f in native['parameters']}
        for group in catalog['groups']:
            for field in group['parameters']:
                if field.get('binding_key'):
                    self.assertIn(field['binding_key'],native_fields)
                    self.assertTrue(field['native_bindings'])

    def test_vendored_sampler_is_the_locked_official_source(self):
        lock=json.loads((ROOT/'catalog/parameter-sources.json').read_text())
        source=next(s for s in lock if s['provider']=='mlx-lm' and s['path']=='mlx_lm/sample_utils.py')
        self.assertEqual(hashlib.sha256((ROOT/'src/runtimes/mlx_sample_utils.py').read_bytes()).hexdigest(),source['sha256'])


if __name__=='__main__':
    unittest.main()
