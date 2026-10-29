"""Validate observed-trace metadata without assuming modeled operation ratios."""
import re,math,json,hashlib

def fields(text):
    return dict(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)",text))

def validate(output,workload,num_rounds,num_run,requested_load):
    def one(pattern):
        rows=re.findall(pattern,output,re.M)
        if len(rows)!=1:raise ValueError('Missing or duplicate real trace metadata')
        return fields(rows[0])
    dataset=one(r'^Real dataset (.+)$')
    profile=one(r'^Real profile=(.+)$')
    header=one(r'^Real (workload=.+)$')
    cluster=int(dataset.get('source_cluster',{'ts':3,'tt':22,'tc':8,'w':0}[workload]))
    prefix={'ts':'Twitter-Storage','tt':'Twitter-Transient','tc':'Twitter-Compute'}
    names={workload:'Webmail' if workload=='w' else prefix[workload]+' (cluster '+str(cluster)+')'}
    if not re.search(r'^Real profile='+re.escape(names[workload])+r' source=',output,re.M):raise ValueError('Wrong trace source cluster')
    n=int(dataset['records']);keys=int(dataset['unique_keys'])
    if dataset['workload']!=workload or not 0<keys<=requested_load or n<=0 or int(dataset['requested_max_keys'])!=requested_load:raise ValueError('Invalid real trace domain')
    if int(dataset['preload_keys'])!=requested_load or int(dataset['background_keys'])!=requested_load-keys:raise ValueError('Invalid preload padding')
    if int(dataset['source_unique_keys'])<keys or int(dataset['source_records'])<n:raise ValueError('Invalid full trace counts')
    for name,value in {'key_bytes':8,'value_bytes':8,'preload':requested_load,'operations_per_round':num_run,'rounds':num_rounds,'source_records':n}.items():
        if int(profile[name])!=value:raise ValueError('Unexpected real trace setting: '+name)
    if not re.fullmatch('[0-9a-f]{64}',dataset['source_sha256']) or dataset['source_sha256']!=profile['source_sha256']:raise ValueError('Missing trace provenance')
    if header['workload']!=workload or header['distribution']!='trace' or any(float(header[x])!=0 for x in ['insert','remove','negative']):raise ValueError('Wrong adapted operation model')
    if not math.isclose(float(header['read'])+float(header['update']),1,abs_tol=1e-9):raise ValueError('Invalid observed ratios')
    audits=[fields(x) for x in re.findall(r'^Real trace (round=.+)$',output,re.M)]
    if len(audits)!=num_rounds:raise ValueError('Missing real trace round audits')
    for i,a in enumerate(audits):
        if int(a['round'])!=i or int(a['reads'])+int(a['updates'])!=num_run or min(int(a['reads']),int(a['updates']))<0:raise ValueError('Invalid observed request counts')
        if int(a['first_cycle'])!=i*num_run//n or int(a['last_cycle'])!=((i+1)*num_run-1)//n:raise ValueError('Invalid trace wrap accounting')
        if not 1<=int(a['key_id_min'])<=int(a['key_id_max'])<=keys:raise ValueError('Trace key outside loaded domain')
        if int(a['generation_ns'])<=0 or a['generation_in_throughput']!='false':raise ValueError('Trace preparation included in measurement')
    return {'keys':keys,'preload_keys':requested_load,'dataset':dataset,'profile':profile,'audits':audits,'ratios':header}

def datasets(repository):
    from pathlib import Path
    import os
    root=Path(os.environ.get('TIMNAH_TRACE_DIR',str(repository/'benchmark/twitter/data')))
    result={}
    for code in ['ts','tt','tc','w']:
        path=root/(code+'.bin');h=hashlib.sha256()
        with path.open('rb') as f:
            for block in iter(lambda:f.read(1048576),b''):h.update(block)
        result[code]={'binary_sha256':h.hexdigest(),'manifest':json.loads(path.with_suffix('.json').read_text())}
    result['validator_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    return result
