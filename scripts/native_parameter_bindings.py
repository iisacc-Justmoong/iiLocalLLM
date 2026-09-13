"""The executable subset of the source catalogs. All other fields remain export-only."""
import hashlib
import re
from pathlib import Path

GENERATION_BINDINGS = {
    'llama.common_params_sampling': {
        'seed':'seed', 'top_k':'top_k', 'top_p':'top_p', 'min_p':'min_p', 'typ_p':'typical_p', 'temp':'temperature',
        'min_keep':'min_keep', 'penalty_last_n':'repetition_context_size', 'penalty_repeat':'repetition_penalty',
        'penalty_freq':'frequency_penalty', 'penalty_present':'presence_penalty', 'xtc_probability':'xtc_probability',
        'xtc_threshold':'xtc_threshold'},
    'transformers.GenerationConfig': {'max_new_tokens':'max_tokens','temperature':'temperature','top_k':'top_k',
        'top_p':'top_p','min_p':'min_p','typical_p':'typical_p','repetition_penalty':'repetition_penalty','stop_strings':'stop'},
    'ollama.Options': {'num_predict':'max_tokens','temperature':'temperature','top_k':'top_k','top_p':'top_p','min_p':'min_p',
        'typical_p':'typical_p','repeat_penalty':'repetition_penalty','repeat_last_n':'repetition_context_size',
        'frequency_penalty':'frequency_penalty','presence_penalty':'presence_penalty','stop':'stop','seed':'seed'},
    'mlx-lm.make_sampler': {'temp':'temperature','top_p':'top_p','top_k':'top_k','min_p':'min_p',
        'min_tokens_to_keep':'min_keep','xtc_probability':'xtc_probability','xtc_threshold':'xtc_threshold'},
    'vllm.SamplingParams': {'max_tokens':'max_tokens','temperature':'temperature','top_k':'top_k','top_p':'top_p',
        'min_p':'min_p','repetition_penalty':'repetition_penalty','frequency_penalty':'frequency_penalty',
        'presence_penalty':'presence_penalty','stop':'stop','seed':'seed','logit_bias':'logit_bias'},
    'sglang.SamplingParams': {'max_new_tokens':'max_tokens','temperature':'temperature','top_k':'top_k','top_p':'top_p',
        'min_p':'min_p','repetition_penalty':'repetition_penalty','frequency_penalty':'frequency_penalty',
        'presence_penalty':'presence_penalty','stop':'stop','seed':'seed','logit_bias':'logit_bias'},
}


def native_group():
    source={'provider':'iiLocalLLM','revision':'0.3.0','path':'Types.h','url':'iiLocalLLM:Types.h',
        'sha256':hashlib.sha256(Path('Types.h').read_bytes()).hexdigest()}
    fields=[]
    def add(name,typ,default,description,**constraints):
        fields.append({'name':name,'native_type':typ,'description':description,'schema':dict(type=typ,**constraints),
            'default':default,'required':False,'source':dict(source,line=1),'native_bindings':['llama.cpp','mlx'],
            'binding_key':name,'binding_note':'generationOptionsFromParameters를 통해 GenerationOptions로 변환하여 실행한다.'})
    add('max_tokens','integer',256,'이번 응답에서 생성할 최대 토큰 수. 프롬프트를 포함한 컨텍스트 한도는 서비스에서 별도로 검사한다.',minimum=1,maximum=1048576)
    add('temperature','number',.7,'로짓의 샘플링 온도. 0은 탐욕 디코딩이며 값이 커질수록 낮은 확률의 토큰을 더 자주 선택한다.',minimum=0,maximum=10)
    add('top_p','number',.9,'누적 확률이 이 값에 도달할 때까지 후보를 유지하는 nucleus sampling. 1은 필터를 끈다.',exclusiveMinimum=0,maximum=1)
    add('top_k','integer',40,'확률 상위 K개 후보를 유지한다. 0은 필터를 끈다.',minimum=0,maximum=1000000)
    add('seed','integer',0,'샘플링 난수 시드. 동일 시드도 다른 런타임이나 장치 사이의 비트 단위 일치를 보장하지 않는다.',minimum=0,maximum=4294967295)
    add('stop','array',[],'출력에서 처음 일치한 문자열 앞에서 생성을 멈춘다. 중지 문자열 자체는 결과에 포함하지 않는다.',maxItems=16,items={'type':'string','minLength':1,'maxLength':1024})
    add('min_p','number',0,'최대 후보 확률에 대한 상대 확률 임계값. 0은 필터를 끈다.',minimum=0,maximum=1)
    add('typical_p','number',1,'국소 엔트로피에 가까운 후보를 유지하는 typical sampling. llama.cpp에서 지원하며 1은 필터를 끈다.',exclusiveMinimum=0,maximum=1)
    add('min_keep','integer',1,'후보 필터에서 최소로 남길 토큰 수. MLX에서는 min-p 필터에 적용된다.',minimum=1,maximum=1048576)
    add('repetition_penalty','number',1,'이전에 출현한 토큰의 로짓에 적용할 부호 보정 반복 패널티. 1은 끄며 1보다 크면 반복을 억제한다.',exclusiveMinimum=0,maximum=100)
    add('repetition_context_size','integer',64,'반복·존재·빈도 패널티가 볼 이전 토큰 수. -1은 전체 기록, 0은 세 패널티를 끈다.',minimum=-1,maximum=1048576)
    add('presence_penalty','number',0,'기록에 한 번 이상 등장한 토큰에서 뺄 가산 로짓 패널티. 음수는 해당 토큰을 선호한다.',minimum=-2,maximum=2)
    add('frequency_penalty','number',0,'기록 내 출현 횟수에 비례하여 뺄 가산 로짓 패널티.',minimum=-2,maximum=2)
    add('xtc_probability','number',0,'XTC 필터를 적용할 확률. 0은 끈다. MLX와 llama.cpp의 필터 순서는 각 런타임을 따른다.',minimum=0,maximum=1)
    add('xtc_threshold','number',.1,'XTC에서 제거 대상 후보를 판별하는 확률 임계값.',minimum=0,maximum=.5)
    add('logit_bias','object',{},'십진수 토큰 ID를 키로 하는 가산 로짓 편향. 토큰 ID가 실제 어휘에 포함되는지도 실행 시 검사한다.',additionalProperties={'type':'number','minimum':-100,'maximum':100})
    for field in fields:
        if field['name']=='typical_p': field['native_bindings']=['llama.cpp']
        native = re.sub(r'_([a-z])',lambda m:m[1].upper(),field['name'])
        text = Path('Types.h').read_text()
        start = text.index('struct GenerationOptions')
        declaration = re.search(r'^\s*(\w+)\s+'+native+r'\b',text[start:],re.M)
        if not declaration: raise ValueError('Native generation binding has no C++ declaration: '+native)
        field['native_type'] = declaration[1]
        field['source']['line'] = text[:start+declaration.end()].count('\n')+1
    return {'id':'iiLocalLLM.GenerationOptions','description':'iiLocalLLM에서 실행하는 공통 생성 제어 객체',
        'phase':'generation','bases':[],'parameters':fields,'source':source}


def apply_bindings(groups):
    groups.append(native_group())
    for group in groups:
        mapping=GENERATION_BINDINGS.get(group['id'],{})
        for field in group['parameters']:
            key=mapping.get(field['name'])
            if key:
                field['binding_key']=key
                field['native_bindings']=['llama.cpp'] if key=='typical_p' else ['llama.cpp','mlx']
                field['binding_note']='명시적으로 설정한 값만 iiLocalLLM 공통 제어로 변환한다. 공통 제약을 재검사하며 원본 기본값이나 전체 백엔드 의미를 가져오지 않는다.'
