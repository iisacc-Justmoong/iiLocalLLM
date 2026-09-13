#!/usr/bin/env python3
"""Extract pinned public configuration declarations without importing/executing upstream code.

The generated catalog keeps provider namespaces, native annotations, literal defaults,
unevaluated default expressions, original documentation, inheritance and source evidence.
Run from the repository root; all downloaded source material belongs below build/.
"""
import argparse
import ast
import copy
import hashlib
import json
from pathlib import Path
import re
from native_parameter_bindings import apply_bindings


UNKNOWN = object()


def literal(node, symbols=None):
    if node is None:
        return UNKNOWN
    if isinstance(node, (list, tuple, dict, bool, int, float, str)):
        return node
    try:
        return ast.literal_eval(node)
    except (ValueError, TypeError):
        pass
    if isinstance(node, ast.Name) and symbols and node.id in symbols:
        return literal(symbols[node.id])
    return UNKNOWN


def cleaned(value):
    return re.sub(r"\s+", " ", value or "").strip()


def json_value(value):
    if isinstance(value, set):
        return [json_value(item) for item in sorted(value, key=repr)]
    if isinstance(value, (list, tuple)):
        return [json_value(item) for item in value]
    if isinstance(value, dict):
        return {str(key): json_value(item) for key, item in value.items()}
    return value


def doc_fields(document):
    fields = {}
    current = None
    for line in (document or "").splitlines():
        match = re.match(r"^\s{0,12}([a-zA-Z_]\w*)\s*\((.+?)\)\s*:\s*(.*)$", line)
        if match:
            current = match[1]
            fields[current] = [match[2], match[3]]
        elif current and re.match(r'^\s{0,8}(Returns?|Examples?|Notes?|Raises?|References?|Attributes):', line):
            current = None
        elif current and line.strip():
            fields[current][1] += " " + line.strip()
    return fields


def union(items):
    flattened = []
    for item in items:
        for branch in item.get("anyOf", [item]):
            if branch not in flattened:
                flattened.append(branch)
    return flattened[0] if len(flattened) == 1 else {"anyOf": flattened}


def annotation_schema(text, symbols=None, visited=None):
    symbols = symbols or {}
    visited = visited or set()
    text = (text or "Any").strip().strip('`')
    text = re.split(r',\s*(?:\*?optional\*?|defaults?\s+to)\b',text)[0]
    text = re.sub(r"\btyping\.", "", text)
    text = re.sub(r"\bOptional\[`?([^`]+)`?\]", r"Optional[\1]", text)
    try:
        node = ast.parse(text, mode="eval").body
    except SyntaxError:
        # Documentation-only annotations use 'or'; retain the native text separately.
        parts = re.split(r"\s+or\s+", text.replace('`', ''))
        if len(parts) > 1:
            return union([annotation_schema(part.split(',')[0], symbols, visited) for part in parts])
        return {"type": "opaque"}

    def schema(n):
        name = ast.unparse(n)
        base = name.split('.')[-1]
        if isinstance(n, ast.Constant):
            if n.value is None:
                return {"type": "null"}
            if isinstance(n.value, str):
                return annotation_schema(n.value, symbols, visited)
        if isinstance(n, ast.BinOp) and isinstance(n.op, ast.BitOr):
            return union([schema(n.left), schema(n.right)])
        if isinstance(n, ast.BoolOp) and isinstance(n.op, ast.Or):
            return union([schema(part) for part in n.values])
        if isinstance(n, ast.Subscript):
            base = ast.unparse(n.value).split('.')[-1]
            args = list(n.slice.elts) if isinstance(n.slice, ast.Tuple) else [n.slice]
            if base in ('Union', 'Optional'):
                return union([schema(arg) for arg in args] + ([{"type": "null"}] if base == 'Optional' else []))
            if base == 'Literal':
                values = [literal(arg, symbols) for arg in args]
                if all(value is not UNKNOWN for value in values):
                    return {"enum": values}
                return {"type": "opaque"}
            if base in ('Annotated', 'A', 'Required', 'NotRequired', 'ClassVar'):
                return schema(args[0])
            if base == 'Callable':
                return {'type':'external'}
            if base in ('list', 'List', 'Sequence', 'Iterable', 'set', 'Set', 'Collection', 'frozenset'):
                return {"type": "array", "items": schema(args[0])}
            if base in ('tuple', 'Tuple'):
                if len(args) == 2 and isinstance(args[1], ast.Constant) and args[1].value is Ellipsis:
                    return {"type": "array", "items": schema(args[0])}
                return {"type": "array", "prefixItems": [schema(arg) for arg in args]}
            if base in ('dict', 'Dict', 'Mapping', 'MutableMapping', 'OrderedDict'):
                return {"type": "object", "additionalProperties": schema(args[-1])}
            return {"type": "opaque"}
        simple = {'bool': 'boolean', 'int': 'integer', 'float': 'number', 'str': 'string',
                  'list': 'array', 'dict': 'object', 'None': 'null', 'NoneType': 'null', 'Path': 'string'}
        if base in simple:
            return {"type": simple[base]}
        if base in ('Tensor','array','Callable','Module','Optimizer','Generator','dtype','device','ProcessGroup'):
            return {'type':'external'}
        if base in symbols and base not in visited:
            symbol = symbols[base]
            if isinstance(symbol, list):
                return {"enum": symbol}
            return annotation_schema(ast.unparse(symbol), symbols, visited | {base})
        if base.endswith(('Config', 'Arguments', 'Args', 'Params', 'Options', 'Plugin', 'Kwargs', 'Setting')):
            return {"type": "object", "ref_name": base}
        return {"type": "opaque"}
    return schema(node)


def phase_for(provider, name):
    name = name.lower()
    if 'quant' in name or 'bitsandbytes' in name or 'gptq' in name or 'awq' in name:
        return 'quantization'
    if provider == 'peft':
        return 'fine_tuning'
    if provider in ('trl', 'torch', 'mlx') or 'training' in name or 'schedule' in name or name in ('adafactor', 'greedylr') or provider == 'mlx-lm' and ('lora' in name or 'train' in name):
        return 'training'
    if provider in ('accelerate', 'deepspeed'):
        return 'infrastructure'
    if any(word in name for word in ('sample', 'sampling', 'generation', 'prediction', 'watermark', 'generate', 'chatrequest')):
        return 'generation'
    if provider in ('vllm', 'sglang'):
        return 'serving'
    return 'loading'


def field_record(name, native_type, value_node, description, source, line, symbols=None, required=False):
    record = {'name': name, 'native_type': native_type or 'Any',
              'description': cleaned(description) or f'공식 {source["provider"]} 설정 필드 {name}. 원본 타입과 기본값 및 연결된 소스 선언을 따른다.',
              'schema': annotation_schema(native_type, symbols), 'required': required,
              'source': dict(source, line=line), 'native_bindings': [],
              'binding_note': '타입 검증과 원본 형식 내보내기를 제공한다. iiLocalLLM 실행 바인딩은 별도 조회한다.'}
    default = value_node
    try:
        annotation = ast.parse(native_type, mode='eval').body
    except SyntaxError:
        annotation = None
    if isinstance(annotation, ast.Subscript) and ast.unparse(annotation.value).split('.')[-1] in ('Annotated', 'A'):
        for metadata in annotation.slice.elts[1:]:
            help_text = literal(metadata, symbols)
            if isinstance(help_text, str): record['description'] = cleaned(help_text)
            if isinstance(metadata, ast.Call):
                keywords = {kw.arg: kw.value for kw in metadata.keywords}
                help_text = literal(keywords.get('help'), symbols)
                if isinstance(help_text, str): record['description'] = cleaned(help_text)
                choices = literal(keywords.get('choices'), symbols)
                if isinstance(choices, (list, tuple)) and choices: record['schema']['enum'] = list(choices)
    if isinstance(value_node, ast.Call) and ast.unparse(value_node.func).split('.')[-1] in ('field', 'Field'):
        keywords = {kw.arg: kw.value for kw in value_node.keywords}
        default = keywords.get('default', value_node.args[0] if value_node.args else None)
        factory = keywords.get('default_factory')
        # Field()/field() without a default (or Field(...)) means required,
        # not a computed default expression to be silently omitted on export.
        required_field = factory is None and (default is None or literal(default, symbols) is Ellipsis)
        if required_field:
            required = True
            default = None
            value_node = None
        if factory is not None:
            factory_name = ast.unparse(factory)
            if factory_name in ('dict', 'list', 'set', 'tuple'):
                record['default'] = {} if factory_name == 'dict' else []
            elif isinstance(factory, ast.Lambda):
                result = literal(factory.body, symbols)
                if result is not UNKNOWN:
                    record['default'] = result
            record['default_expression'] = ast.unparse(value_node)
        if 'init' in keywords and literal(keywords['init']) is False:
            record['read_only'] = True
        for native, key in [('ge', 'minimum'), ('le', 'maximum'), ('gt', 'exclusiveMinimum'), ('lt', 'exclusiveMaximum'),
                            ('min_length', 'minLength'), ('max_length', 'maxLength')]:
            if native in keywords:
                bound = literal(keywords[native], symbols)
                if isinstance(bound, (int, float)):
                    record['schema'][key] = bound
        if 'metadata' in keywords:
            metadata = literal(keywords['metadata'], symbols)
            if isinstance(metadata, dict) and isinstance(metadata.get('help'), str):
                record['description'] = cleaned(metadata['help'])
            if isinstance(metadata, dict) and 'choices' in metadata:
                record['schema']['enum'] = metadata['choices']
    result = literal(default, symbols)
    if result is not UNKNOWN:
        if isinstance(result, (set, tuple)):
            result = list(result)
        if result is not Ellipsis:
            record['default'] = result
            # Several upstream libraries intentionally use null before post-init resolution,
            # even when a non-Optional field annotation is retained for the resolved object.
            if result is None:
                record['schema']['nullable'] = True
    if value_node is not None and 'default' not in record:
        record['default_expression'] = ast.unparse(value_node)
    record['required'] = required and 'default' not in record and 'default_expression' not in record
    record['sensitive'] = bool(re.search(r'(?:^|_)(?:api_key|password|secret|auth_token|hub_token|hf_token)$', name))
    return record


def python_groups(source, root, shared_symbols=None):
    text = (root / source['provider'] / source['path']).read_text()
    tree = ast.parse(text)
    symbols = dict(shared_symbols or {})
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            symbols[node.targets[0].id] = node.value
        if isinstance(node, ast.ClassDef) and any('Enum' in ast.unparse(base) for base in node.bases):
            values = []
            for field in node.body:
                if isinstance(field, (ast.Assign, ast.AnnAssign)):
                    value = literal(field.value)
                    if value is not UNKNOWN:
                        values.append(value)
            if values:
                symbols[node.name] = values
    groups = []
    for node in tree.body:
        if not isinstance(node, ast.ClassDef) or node.name.startswith('_') and node.name != '_BaseConfig' or any('Enum' in ast.unparse(base) for base in node.bases):
            continue
        if not (re.search(r'(Config|Args|Arguments|Params|Parameters|Plugin|Kwargs|Options)$', node.name)
                or node.name in ('HfDeepSpeedConfig', 'Adafactor', 'GreedyLR')
                or source['provider'] in ('torch', 'mlx') and node.name not in ('Optimizer', 'LRScheduler')
                or '/arg_groups/fields/' in source['path']):
            continue
        fields = {}
        docs = doc_fields(ast.get_docstring(node))
        for index, declaration in enumerate(node.body):
            if isinstance(declaration, ast.AnnAssign) and isinstance(declaration.target, ast.Name):
                name = declaration.target.id
                native_type = ast.unparse(declaration.annotation)
                if name.startswith('_') or 'ClassVar' in native_type:
                    continue
                description = docs.get(name, ['', ''])[1]
                if index + 1 < len(node.body) and isinstance(node.body[index + 1], ast.Expr):
                    value = node.body[index + 1].value
                    if isinstance(value, ast.Constant) and isinstance(value.value, str):
                        description = value.value
                fields[name] = field_record(name, native_type, declaration.value, description, source,
                                            declaration.lineno, symbols, required=declaration.value is None)
        initializer = next((item for item in node.body if isinstance(item, ast.FunctionDef) and item.name == '__init__'), None)
        if initializer:
            positional = initializer.args.posonlyargs + initializer.args.args
            defaults = [None] * (len(positional) - len(initializer.args.defaults)) + initializer.args.defaults
            args = list(zip(positional, defaults)) + list(zip(initializer.args.kwonlyargs, initializer.args.kw_defaults))
            for arg, default in args:
                if arg.arg == 'self' or arg.arg.startswith('_'):
                    continue
                doc_type, description = docs.get(arg.arg, ['', ''])
                native_type = ast.unparse(arg.annotation) if arg.annotation else doc_type.split(',')[0].replace('`', '')
                if not native_type:
                    value = literal(default, symbols)
                    native_type = type(value).__name__ if value is not UNKNOWN and value is not None else 'Any'
                fields.setdefault(arg.arg, field_record(arg.arg, native_type, default, description, source, arg.lineno, symbols, default is None))
            for assignment in ast.walk(initializer):
                if not isinstance(assignment, (ast.Assign, ast.AnnAssign)):
                    continue
                targets = assignment.targets if isinstance(assignment, ast.Assign) else [assignment.target]
                if len(targets) != 1 or not isinstance(targets[0], ast.Attribute):
                    continue
                target = targets[0]
                call = assignment.value
                if (not isinstance(target.value, ast.Name) or target.value.id != 'self'
                        or not isinstance(call, ast.Call) or not isinstance(call.func, ast.Attribute)
                        or call.func.attr != 'pop' or not isinstance(call.func.value, ast.Name)
                        or call.func.value.id != 'kwargs' or not call.args or not isinstance(call.args[0], ast.Constant)):
                    continue
                name = call.args[0].value
                if not isinstance(name, str) or name.startswith('_'):
                    continue
                native_type, description = docs.get(name, ['', ''])
                native_type = native_type.split(', *optional*')[0].replace('`', '') or 'Any'
                default = call.args[1] if len(call.args) > 1 else None
                fields.setdefault(name, field_record(name, native_type, default, description, source, assignment.lineno, symbols))
        bases = [ast.unparse(base).split('.')[-1] for base in node.bases]
        if node.name == 'ServerArgs' and '_INPUT_NAMESPACES' in symbols:
            bases = [ast.unparse(item) for item in symbols['_INPUT_NAMESPACES'].elts]
        if fields or node.name == 'ServerArgs':
            groups.append({'id': source['provider'] + '.' + node.name, 'class': node.name,
                'description': cleaned((ast.get_docstring(node) or '').split('\n\n')[0]) or node.name,
                'phase': phase_for(source['provider'], node.name),
                'bases': bases,
                'parameters': list(fields.values()), 'source': source})
    # Function-based configuration APIs have the same object contract as constructors.
    function_names = {'make_sampler', 'make_logits_processors', 'generate_step', 'stream_generate',
        'speculative_generate_step', 'batch_generate', 'load', 'load_model', 'quantize_model',
        'linear_to_lora_layers', 'build_schedule'} if source['provider'] == 'mlx-lm' else set()
    for node in tree.body:
        if not isinstance(node, ast.FunctionDef) or node.name.startswith('_'): continue
        if not (node.name in function_names or source['provider'] == 'transformers' and 'schedule' in node.name
                or source['provider'] == 'mlx' and source['path'].endswith('schedulers.py')): continue
        docs = doc_fields(ast.get_docstring(node)); fields = []
        positional = node.args.posonlyargs + node.args.args
        defaults = [None] * (len(positional) - len(node.args.defaults)) + node.args.defaults
        for arg, default in list(zip(positional, defaults)) + list(zip(node.args.kwonlyargs, node.args.kw_defaults)):
            if arg.arg in ('model', 'tokenizer', 'prompt', 'optimizer', 'draft_model', 'inputs', 'input_embeddings'): continue
            native, description = docs.get(arg.arg, ['', ''])
            if arg.annotation: native = ast.unparse(arg.annotation)
            if not native:
                value = literal(default, symbols)
                native = type(value).__name__ if value is not UNKNOWN and value is not None else 'Any'
            fields.append(field_record(arg.arg, native.split(', *optional*')[0], default, description, source, arg.lineno, symbols, default is None))
        if fields: groups.append({'id':source['provider']+'.'+node.name, 'class':node.name,
            'description':cleaned((ast.get_docstring(node) or node.name).split('\n\n')[0]),
            'phase':phase_for(source['provider'], node.name), 'bases':[], 'parameters':fields, 'source':source})
    if source['provider'] == 'mlx-lm' and Path(source['path']).name in ('lora.py', 'generate.py'):
        fields = {}; config = literal(symbols.get('CONFIG_DEFAULTS'), symbols)
        config = config if isinstance(config, dict) else {}
        for call in ast.walk(tree):
            if not isinstance(call, ast.Call) or not isinstance(call.func, ast.Attribute) or call.func.attr != 'add_argument': continue
            flags = [literal(arg) for arg in call.args]; flags = [f for f in flags if isinstance(f,str) and f.startswith('--')]
            if not flags: continue
            keywords = {kw.arg:kw.value for kw in call.keywords}
            name = literal(keywords.get('dest'))
            if not isinstance(name,str): name = flags[0][2:].replace('-','_')
            native = ast.unparse(keywords['type']) if 'type' in keywords else 'str'
            action = literal(keywords.get('action'))
            if action in ('store_true','store_false'): native='bool'
            default = keywords.get('default')
            if name in config: default = ast.parse(repr(config[name]), mode='eval').body
            record = field_record(name,native,default,literal(keywords.get('help')) if isinstance(literal(keywords.get('help')),str) else '',source,call.lineno,symbols)
            choices = literal(keywords.get('choices'),symbols)
            if isinstance(choices,(list,tuple)): record['schema']['enum']=list(choices)
            nargs=literal(keywords.get('nargs'))
            if nargs in ('*','+') or isinstance(nargs,int): record['schema']={'type':'array','items':record['schema']}
            fields[name]=record
        for name,value in config.items():
            if name not in fields: fields[name]=field_record(name,type(value).__name__,ast.parse(repr(value),mode='eval').body,
                'Default configuration entry from CONFIG_DEFAULTS.',source,symbols['CONFIG_DEFAULTS'].lineno,symbols)
        if fields:
            name = 'LoraArguments' if source['path'].endswith('lora.py') else 'GenerateArguments'
            groups.append({'id':'mlx-lm.'+name,'class':name,'description':name,'phase':phase_for('mlx-lm',name),'bases':[], 'parameters':list(fields.values()),'source':source})
    if source['provider']=='deepspeed' and source['path']=='deepspeed/runtime/config.py':
        sections={}
        for call in ast.walk(tree):
            if not isinstance(call,ast.Call) or not isinstance(call.func,ast.Name) or call.func.id!='get_scalar_param' or len(call.args)<3: continue
            container=call.args[0]; section='TrainConfig'
            if isinstance(container,ast.Subscript) and ast.unparse(container.value)=='param_dict':
                section=literal(container.slice,symbols)
                if not isinstance(section,str): continue
            elif ast.unparse(container)!='param_dict': continue
            name=literal(call.args[1],symbols)
            if not isinstance(name,str): continue
            default=literal(call.args[2],symbols)
            native=type(default).__name__ if default is not UNKNOWN and default is not None else 'Any'
            record=field_record(name,native,call.args[2],f'DeepSpeed JSON {section}.{name}; declared by get_scalar_param.',source,call.lineno,symbols)
            sections.setdefault(section,{})[name]=record
        top=sections.setdefault('TrainConfig',{})
        for section in list(sections):
            if section=='TrainConfig': continue
            record=field_record(section,'dict',None,'Nested DeepSpeed training configuration.',source,1)
            record['schema']={'type':'object','ref_name':'TrainConfig_'+section}
            top[section]=record
        for section,fields in sections.items():
            name=section if section=='TrainConfig' else 'TrainConfig_'+section
            groups.append({'id':'deepspeed.'+name,'class':name,'description':'DeepSpeed JSON training configuration',
                'phase':'training','bases':[],'parameters':list(fields.values()),'source':source})
    return groups


def cpp_groups(source, path):
    text = path.read_text(); groups = []
    enums = {}
    for match in re.finditer(r'enum(?:\s+class)?\s+(\w+)\s*\{(.*?)\}', text, re.S):
        names = re.findall(r'(?:^|,)\s*(\w+)\s*(?:=[^,]+)?', re.sub(r'//[^\n]*', '', match[2]))
        if names: enums[match[1]] = names
    for match in re.finditer(r'(?:typedef\s+)?struct\s+((?:common|llama)_\w*(?:params|params_\w+|options|config))\s*\{', text):
        start = match.end(); level = 1; end = start
        while end < len(text) and level:
            level += (text[end] == '{') - (text[end] == '}'); end += 1
        body = text[start:end - 1]; fields = []
        # Initializers can span lines (notably the ordered sampler chain). Never stop at a newline.
        pattern = r'^[ \t]*(?!static\b|return\b|using\b)([\w:*<>,&][\w \t:*<>,&]*?)[ \t]+(\w+)[ \t]*(\[[^]\n]+\])?[ \t]*(?:=[ \t]*([^;]+))?;[ \t]*(?://([^\n]*))?'
        for field in re.finditer(pattern, body, re.M):
            native_type, name, array, default, description = field.groups()
            native_type = native_type.strip() + (array or '')
            if not description:
                comments = []
                for line in reversed(body[:field.start()].splitlines()):
                    if not line.strip().startswith('//'): break
                    comments.insert(0,line.strip()[2:].strip())
                description = ' '.join(comments)
            base = native_type.removeprefix('enum ').removeprefix('struct ')
            if base in enums: schema = {'enum':enums[base]}
            elif 'bool' in native_type: schema = {'type':'boolean'}
            elif re.search(r'float|double', native_type): schema = {'type':'number'}
            elif re.search(r'int\d*_t|size_t|\bint\b|llama_token', native_type): schema = {'type':'integer'}
            elif native_type in ('std::string','string') or 'char' in native_type: schema = {'type':'string'}
            elif re.fullmatch(r'(?:common|llama)_\w*(?:params|params_\w+|options|config)',base): schema = {'type':'object','ref_name':base}
            else: schema = {'type':'opaque'}
            if 'vector' in native_type or array: schema = {'type':'array'}
            if 'map<' in native_type or 'unordered_map<' in native_type: schema = {'type':'object'}
            record = field_record(name,native_type,None,description,source,text[:start+field.start()].count('\n')+1)
            record['schema'] = schema
            if default:
                expression = re.sub(r'//[^\n]*','',default).strip()
                converted = re.sub(r'(?<=\d)[fFuUlL]+\b','',expression).replace('true','True').replace('false','False')
                if schema.get('type')=='array' and converted.startswith('{'): converted='['+converted[1:-1]+']'
                try: value = ast.literal_eval(converted)
                except (SyntaxError,ValueError): value = UNKNOWN
                if value is UNKNOWN or isinstance(value,int) and not -(2**63)<=value<2**63:
                    record['default_expression']=expression
                else: record['default']=value
            fields.append(record)
        if fields:
            groups.append({'id':'llama.'+match[1], 'class':match[1], 'description':match[1],
                'phase':phase_for('llama',match[1]), 'bases':[], 'parameters':fields, 'source':source})
    return groups


def go_groups(source, path):
    text = path.read_text(); groups = []
    for match in re.finditer(r'type\s+(\w+)\s+struct\s*\{(.*?)\n\}', text, re.S):
        if match[1] not in ('Options', 'Runner', 'GenerateRequest', 'ChatRequest', 'EmbedRequest', 'ThinkValue', 'Format', 'Tool', 'ToolFunction'):
            continue
        fields = []; comments = []; bases = []
        for offset, line in enumerate(match[2].splitlines()):
            if line.strip().startswith('//'):
                comments.append(line.strip()[2:].strip()); continue
            field = re.match(r'\s*(\w+)\s+([^`]+)`json:"([^",]+)([^\"]*)"`', line)
            if not field:
                if re.match(r'^\s*[A-Z]\w*\s*$', line): bases.append(line.strip())
                continue
            name, native, wire, optional = field.groups(); native = native.strip()
            record = field_record(wire, native, None, ' '.join(comments), source, text[:match.start(2)].count('\n') + offset + 1)
            comments = []
            base = native.lstrip('*')
            schema = {'type': {'int':'integer','int64':'integer','float32':'number','float64':'number','bool':'boolean','string':'string'}.get(base,'opaque')}
            if base.startswith('[]'): schema = {'type':'array','items':annotation_schema(base[2:])}
            if base.startswith('map['): schema = {'type':'object'}
            if native.startswith('*'): schema['nullable'] = True
            record['schema'] = schema; record['required'] = not optional and not native.startswith('*')
            fields.append(record)
        if fields: groups.append({'id':'ollama.' + match[1], 'class':match[1], 'description':match[1], 'phase':phase_for('ollama',match[1]), 'bases':bases, 'parameters':fields, 'source':source})
    return groups


def split_top(text, separator):
    parts=[]; start=0; level=0; quote=None; escaped=False
    for i,c in enumerate(text):
        if quote:
            if c==quote and not escaped: quote=None
            escaped = c=='\\' and not escaped
            continue
        if c in ('"',"'",'`'): quote=c
        elif c in '[{(<': level+=1
        elif c in ']})>': level-=1
        elif c==separator and level==0: parts.append(text[start:i].strip()); start=i+1
    parts.append(text[start:].strip())
    return [p for p in parts if p]


def ts_declarations(text):
    clean = re.sub(r'/\*.*?\*/|//[^\n]*',lambda m:''.join('\n' if c=='\n' else ' ' for c in m[0]),text,flags=re.S)
    result={}
    for match in re.finditer(r'export\s+(interface|type)\s+(\w+)(?:<[^>]+>)?(?:\s+extends\s+([^={]+))?\s*([={])',clean):
        start=match.end(); kind=match[1]
        if kind=='interface': start-=1
        tail=clean[start:]
        if kind=='interface':
            level=0; stop=0
            for stop,c in enumerate(tail):
                level+=(c=='{')-(c=='}')
                if level==0: break
            body=text[start:start+stop+1]
        else:
            fragment=split_top(tail,';')[0]
            body=text[start:start+len(tail)-len(tail.lstrip())+len(fragment)].strip()
        result[match[2]]=(body,start,match[3] or '')
    return result


def ts_schema(native,symbols,seen=None):
    seen=seen or set(); native=native.strip().removeprefix('readonly ').strip()
    native=re.sub(r'/\*.*?\*/|//[^\n]*','',native,flags=re.S).strip()
    branches=split_top(native,'|')
    if len(branches)>1: return union([ts_schema(p,symbols,seen) for p in branches])
    if native.startswith('(') and native.endswith(')'): return ts_schema(native[1:-1],symbols,seen)
    if native.endswith('[]'): return {'type':'array','items':ts_schema(native[:-2],symbols,seen)}
    if native in ('false','true'): return {'enum':[native=='true']}
    if native in ('undefined','null'): return {'type':'null'}
    if native in ('number','string','boolean'): return {'type':{'number':'number','string':'string','boolean':'boolean'}[native]}
    if re.fullmatch(r'"[^"\n]*"|\'[^\'\n]*\'|[0-9]+',native): return {'enum':[ast.literal_eval(native)]}
    if native.startswith('[') and native.endswith(']'): return {'type':'array','prefixItems':[ts_schema(p,symbols,seen) for p in split_top(native[1:-1],',')]}
    if native.startswith('{') and native.endswith('}'):
        properties={}; required=[]
        for declaration in split_top(native[1:-1],';'):
            match=re.match(r'(\w+)(\?)?\s*:\s*(.*)',declaration,re.S)
            if not match: return {'type':'object'}
            properties[match[1]]=ts_schema(match[3],symbols,seen)
            if not match[2]: required.append(match[1])
        return {'type':'object','properties':properties,'required':required,'additionalProperties':False}
    if native.startswith(('Array<','ReadonlyArray<')) and native.endswith('>'):
        return {'type':'array','items':ts_schema(native[native.index('<')+1:-1],symbols,seen)}
    if native.startswith('Record<') and native.endswith('>'):
        args=split_top(native[7:-1],','); return {'type':'object','additionalProperties':ts_schema(args[-1],symbols,seen)}
    name=native.split('<')[0]
    if name in symbols and name not in seen:
        body=symbols[name][0]
        if body.strip().startswith('{') and len(split_top(body,'|'))==1: return {'type':'object','ref_name':name}
        return ts_schema(body,symbols,seen|{name})
    return {'type':'opaque'}


def ts_groups(source,path,symbols=None):
    text=path.read_text(); declarations=ts_declarations(text); symbols=dict(symbols or {},**declarations); groups=[]
    for name,(body,start,bases) in declarations.items():
        if not body.startswith('{') or not body.endswith('}') or len(split_top(re.sub(r'/\*.*?\*/','',body,flags=re.S),'|'))!=1: continue
        fields=[]
        # Preserve comments for descriptions, then split only top-level properties.
        without_comments=re.sub(r'/\*.*?\*/|//[^\n]*',lambda m:''.join('\n' if c=='\n' else ' ' for c in m[0]),body,flags=re.S)
        offset=1
        for part in split_top(without_comments[1:-1],';'):
            position=without_comments.find(part,offset); original=body[offset:position+len(part)]
            field=re.match(r'(?:readonly\s+)?(\w+)(\?)?\s*:\s*(.*)',part,re.S)
            offset=position+len(part)+1
            if not field: continue
            docs=re.findall(r'/\*\*(.*?)\*/',original,re.S)
            description=re.sub(r'^\s*\* ?','',docs[-1] if docs else '',flags=re.M)
            record=field_record(field[1],field[3],None,description,source,text[:start+position].count('\n')+1,required=not field[2])
            record['schema']=ts_schema(field[3],symbols)
            fields.append(record)
        if fields: groups.append({'id':'lmstudio.'+name,'class':name,'description':name,'phase':phase_for('lmstudio',name),
            'bases':[b.strip().split('<')[0] for b in bases.split(',') if b.strip()],'parameters':fields,'source':source})
    return groups


def generate(root, manifest):
    groups = []; shared = {}; ts_symbols = {}
    for source in manifest:
        path = root / source['provider'] / source['path']
        if hashlib.sha256(path.read_bytes()).hexdigest() != source['sha256']:
            raise ValueError(f'Source checksum mismatch: {path}')
        if path.suffix == '.py':
            symbols = shared.setdefault(source['provider'], {})
            for node in ast.parse(path.read_text()).body:
                if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
                    symbols[node.targets[0].id] = node.value
                if isinstance(node, ast.ClassDef) and any('Enum' in ast.unparse(base) for base in node.bases):
                    values = [literal(item.value) for item in node.body if isinstance(item, (ast.Assign, ast.AnnAssign))]
                    values = [v for v in values if v is not UNKNOWN]
                    if values: symbols[node.name] = values
        elif path.suffix == '.ts': ts_symbols.update(ts_declarations(path.read_text()))
    for source in manifest:
        path = root / source['provider'] / source['path']
        if hashlib.sha256(path.read_bytes()).hexdigest() != source['sha256']:
            raise ValueError(f'Source checksum mismatch: {path}')
        suffix = path.suffix
        if suffix == '.py': groups.extend(python_groups(source,root,shared.get(source['provider'])))
        elif suffix == '.go': groups.extend(go_groups(source,path))
        elif suffix == '.ts' and not path.name.endswith('.test.ts'): groups.extend(ts_groups(source,path,ts_symbols))
        elif suffix == '.h' and source['provider'] == 'llama': groups.extend(cpp_groups(source,path))
    ids = {}
    for group in groups:
        if group['id'] in ids:
            group['id'] = group['source']['provider'] + '.' + group['source']['path'].replace('/','.').removesuffix('.py') + '.' + group['class']
        ids[group['id']] = group
    def resolve(name, group):
        local = group['source']['provider'] + '.' + name
        if local in ids: return local
        if 'transformers.'+name in ids: return 'transformers.'+name
        candidates=[key for key in ids if key.endswith('.'+name)]
        return candidates[0] if len(candidates)==1 else None
    completed=set()
    def inherit(group, stack=None):
        if group['id'] in completed: return
        stack=(stack or set()) | {group['id']}
        inherited={}
        for base in group['bases']:
            if group['source']['provider'] in ('torch', 'mlx'): continue
            reference=resolve(base,group)
            if reference and reference not in stack:
                inherit(ids[reference],stack)
                inherited.update({field['name']:copy.deepcopy(field) for field in ids[reference]['parameters']})
        inherited.update({field['name']:field for field in group['parameters']})
        group['parameters']=list(inherited.values()); completed.add(group['id'])
    for group in groups: inherit(group)
    def references(schema,group):
        if 'ref_name' in schema:
            ref=resolve(schema.pop('ref_name'),group)
            if ref: schema['ref']=ref
        for value in schema.values():
            if isinstance(value,dict): references(value,group)
            elif isinstance(value,list):
                for item in value:
                    if isinstance(item,dict): references(item,group)
    for group in groups:
        for field in group['parameters']:
            references(field['schema'],group)
            if 'default' in field:
                field['default']=json_value(field['default'])
                if field['schema'].get('type')=='array' and field['default']=={}:
                    field['default']=[]
        group.pop('class',None)
    apply_bindings(groups)
    return {'schema_version':1,'snapshot_date':'2026-09-13','groups':sorted(groups,key=lambda g:g['id'])}


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--sources',type=Path,default=Path('build/parameter-sources'))
    parser.add_argument('--output',type=Path,default=Path('catalog/parameters.json'))
    parser.add_argument('--coverage',type=Path,default=Path('catalog/parameter-coverage.json'))
    args=parser.parse_args()
    manifest=json.loads((args.sources/'sources.json').read_text())
    catalog=generate(args.sources,manifest)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(catalog,ensure_ascii=False,indent=2)+'\n')
    counts={}
    for group in catalog['groups']:
        provider=group['id'].split('.')[0]
        counts[provider]=counts.get(provider,0)+len(group['parameters'])
    print(json.dumps({'groups':len(catalog['groups']),'parameters':sum(counts.values()),'providers':counts},indent=2))
    unique={(f['source']['provider'],f['source']['path'],f['source']['line'],f['name']) for g in catalog['groups'] for f in g['parameters']}
    report={'schema_version':1,'snapshot_date':catalog['snapshot_date'],'groups':len(catalog['groups']),
        'fields_including_inheritance':sum(counts.values()),'unique_declarations':len(unique),'providers':counts,
        'sources':[dict(s,groups=[g['id'] for g in catalog['groups'] if g['source']['provider']==s['provider'] and g['source']['path']==s['path']]) for s in manifest],
        'opaque_fields':[g['id']+'.'+f['name'] for g in catalog['groups'] for f in g['parameters'] if f['schema'].get('type')=='opaque'],
        'scope':'Pinned public configuration declarations, selected function/CLI controls, and their inheritance. Runtime-dependent expressions, arbitrary backend validation, and undeclared plugin/model-specific kwargs are not executed or enumerated.'}
    args.coverage.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')


if __name__=='__main__':
    main()
