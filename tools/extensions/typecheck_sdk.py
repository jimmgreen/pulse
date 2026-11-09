"""Compile source or extracted SDK declarations and template examples with the pinned TypeScript compiler."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
NEGATIVE = [
    'const removedRuntimeInvoke: ExtensionCommand = {id:"r",title:"Run",capability:"runtime.invoke",args:{entrypoint:"run"}};',
    'const removedRuntime: ExtensionManifest = {apiVersion:1,id:"x",name:"X",commands:[],runtime:{kind:"javascript",version:1,source:"function run() {}",entrypoints:["run"]}};',
    'const renameByPath: ResourceRequest = {op:"files.prepareRename",items:[{path:"C:/x",newLeafName:"a"}]};',
    'const renameWithoutPlan: ResourceRequest = {op:"files.commitRename",items:[]};',
    'const renameWrongPlanType: ResourceRequest = {op:"files.commitRename",planId:42};',
    'const renameWrongStatus: ResourceResponse<"files.commitRename"> = {ok:true,data:{taskId:"1",status:"completed"}};',

    'const tabNewArgs: ExtensionCommand = {id:"n",title:"New",capability:"tabs.new",args:{}};',
    'const tabDuplicatePath: ExtensionCommand = {id:"d",title:"Duplicate",capability:"tabs.duplicate",args:{path:"C:/private"}};',
    'const tabCloseOwner: ExtensionCommand = {id:"c",title:"Close",capability:"tabs.closeActive",args:{owner:123}};',
    'const tabActivateNoIndex: ExtensionCommand = {id:"a",title:"Activate",capability:"tabs.activate",args:{}};',
    'const tabActivateString: ExtensionCommand = {id:"a",title:"Activate",capability:"tabs.activate",args:{index:"1"}};',
    'const paneFocusNoArgs: ExtensionCommand = {id:"p",title:"Focus",capability:"panes.focus"};',
    'const paneFocusHandle: ExtensionCommand = {id:"p",title:"Focus",capability:"panes.focus",args:{index:0,handle:123}};',
    'const selectionArgs: ExtensionCommand = {id:"a",title:"All",capability:"selection.selectAll",args:{}};',
    'const selectionPath: ExtensionCommand = {id:"a",title:"Invert",capability:"selection.invert",args:{path:"C:/private"}};',
    'const selectionClearArgs: ExtensionCommand = {id:"a",title:"Clear",capability:"selection.clear",args:undefined};',
    'const sortNoArgs: ExtensionCommand = {id:"s",title:"Sort",capability:"view.setSort"};',
    'const sortNoDirection: ExtensionCommand = {id:"s",title:"Sort",capability:"view.setSort",args:{column:"name"}};',
    'const sortUnknownColumn: ExtensionCommand = {id:"s",title:"Sort",capability:"view.setSort",args:{column:"owner",direction:"ascending"}};',
    'const sortWrongDirection: ExtensionCommand = {id:"s",title:"Sort",capability:"view.setSort",args:{column:"size",direction:"desc"}};',
    'const sortArbitraryTarget: ExtensionCommand = {id:"s",title:"Sort",capability:"view.setSort",args:{column:"name",direction:"ascending",paneId:5}};',
    'const localizedView: ResourceResponse<"context.get"> = {ok:true,data:{directory:"",selectionCount:0,selectionName:"",viewMode:"Details",loading:false}};',
    'const emptyView: ResourceResponse<"context.get"> = {ok:true,data:{directory:"",selectionCount:0,selectionName:"",viewMode:"",loading:false}};',

    'const wrongResponse: ResourceResponse<"settings.get"> = {ok:true,data:{accepted:true,taskId:"1"}};',
    'const numericFileTime: ResourceResponse<"files.stat"> = {ok:true,data:{name:"x",sizeBytes:"0",directory:false,reparsePoint:false,attributes:0,modifiedFileTime:1}};',
    'const numericBytes: ResourceResponse<"operations.get"> = {ok:true,data:{taskId:"1",cancelRequested:false,terminal:false,completed:false,transferredBytes:1,totalBytes:"2",items:[]}};',
    'const badItemState: ResourceResponse<"operations.get"> = {ok:true,data:{taskId:"1",cancelRequested:false,terminal:true,completed:false,transferredBytes:"0",totalBytes:"2",items:[{source:"a",destination:"b",state:6,error:0,cleanupError:0}]}};',
    'const badSettingValue: ResourceResponse<"settings.set"> = {ok:true,data:{version:0,values:{x:42}}};',
    'const missingSettings: ResourceResponse<"settings.get"> = {ok:true,data:{version:0}};',
    'const fakeResponseOp: ResourceResponse<"operations.cancel"> = {op:"operations.cancel",ok:true,data:{accepted:true,taskId:"1"}};',
    'const errorWithoutCode: ResourceResponse<"context.get"> = {ok:false,error:{message:"no"}};',

    'const workflowCancelArgs: ExtensionCommand = {id:"c",title:"C",capability:"workflow.cancel",args:{}};',
    'const workflowSourceFilter: UiNode = {type:"virtualList",id:"v",dataSource:{source:"context.workflow",sortState:"sort"}};',

    'const resourcePath: ResourceRequest = {op:"files.list",path:"C:/private"};',
    'const resourceCaller: ResourceRequest = {op:"context.get",packageId:"other"};',
    'const numericTask: ResourceRequest = {op:"operations.cancel",taskId:123};',
    'const settingsNoCas: ResourceRequest = {op:"settings.set",key:"x",value:"y"};',
    'const settingsObject: ResourceRequest = {op:"settings.set",key:"x",value:{},expectedVersion:0};',

    'const badWorkflowArgs: ExtensionCommand = {id:"w",title:"W",capability:"workflow.run",args:{steps:["x"]}};',
    'const unknownResource: ExtensionManifest = {apiVersion:1,id:"x",name:"X",commands:[],resources:["files.anyPath"]};',
    'const workflowScript: ExtensionManifest = {apiVersion:1,id:"x",name:"X",commands:[],workflows:[{id:"w",steps:["x"],script:"evil"}]};',

    'const flexOnRoot: ExtensionPanel = {title:"Bad",ui:{version:1,root:{type:"text",id:"r",flex:1}}};',
    'const flexInGrid: UiNode = {type:"grid",id:"g",children:[{type:"text",id:"t",flex:1}]};',
    'const clippedContainer: UiNode = {type:"group",id:"g",maxHeight:100};',

    'const invalidFlexType: UiNode = {type:"text",id:"t",flex:"grow"};',
    'const invalidWidthType: UiNode = {type:"text",id:"t",minWidth:"140px"};',
    'const invalidMaxHeightType: UiNode = {type:"group",id:"g",maxHeight:null};',

    'const validationNoState: UiNode = {type:"textInput",id:"i",text:"Input",validation:{required:true,message:"Required"}};',
    'const mixedRowsAndSource: UiNode = {type:"virtualList",id:"v",rows:[],dataSource:{source:"context.directoryEntries"}};',
    'const arbitrarySource: UiNode = {type:"virtualList",id:"v",dataSource:{source:"C:/private"}};',
    'const taskFilter: UiNode = {type:"virtualList",id:"v",dataSource:{source:"context.transferTask",filterState:"q"}};',
    'const taskPageSize: UiNode = {type:"virtualList",id:"v",dataSource:{source:"context.transferTask",pageSize:64}};',
    'const actionNoEffect: ExtensionPanel = {title:"A",ui:{version:1,root:{type:"text",id:"r"},actions:[{id:"a",validate:["i"]}]}};',

    'const badLocalUpdate: ExtensionPanel = {title:"Local",ui:{version:1,root:{type:"text",id:"t"},actions:[{id:"a",updates:[{key:"x",value:false}]}]}};',
    'const badLocalExecute: ExtensionPanel = {title:"Local",ui:{version:1,root:{type:"text",id:"t"},actions:[{id:"a",updates:[],execute:"cmd.exe"}]}};',

    'const missingTitleAction: UiNode = {type:"titleBarButton",id:"t",role:"close"};',
    'const wrongTitleRole: UiNode = {type:"titleBarButton",id:"t",role:"execute",command:"x"};',
    'const wrongSplitState: UiNode = {type:"splitPane",id:"s",state:50,children:[]};',
    'const privateHandle: UiNode = {type:"textInput",id:"i",text:"Name",hwnd:123};',
    'const privateAnimation: UiNode = {type:"progress",id:"p",animation_progress:0.5};',
    'const wrongDriveBytes: UiNode = {type:"driveItem",id:"d",freeBytes:"100"};',

    'const toggleWithoutState: UiNode = {type:"button",id:"t",kind:"toggle"};',
    'const mixedMenuItem: UiNode = {type:"menuButton",id:"m",menu:[{id:"i",text:"I",command:"x",children:[{id:"c",text:"C",command:"x"}]}]};',
    'const wrongSliderState: UiNode = {type:"slider",id:"s",state:1};',
    'const wrongInfoKind: UiNode = {type:"infoBar",id:"i",kind:"custom"};',
    'const badTableCells: UiNode = {type:"dataTable",id:"t",columns:[{id:"c",text:"C"}],rows:[{id:"r",cells:[5]}]};',

    'const badSwitchState: UiNode = {type:"switch", id:"s", state:true};',
    'const badButtonKind: UiNode = {type:"button", id:"b", command:"x", kind:"custom"};',
    'const badProgressStatus: UiNode = {type:"progress", id:"p", status:"failed"};',
    'const badProgressKind: UiNode = {type:"progress", id:"p", kind:"pie"};',
    'const badPlaceholder: UiNode = {type:"textInput", id:"i", text:"Name", placeholder:42};',

    'const mixedPanel: ExtensionPanel = {title:"Bad", commands:["x"], ui:{version:1, root:{id:"x",type:"text"}}};',

    'const missingNodeId: UiNode = {type:"text", text:"Missing ID"};',
    'const leafChildren: UiNode = {type:"text", id:"x", children:[]};',
    'const wrongUiCommand: UiNode = {type:"button", id:"x", command:42};',
    'const unknownUiType: UiNode = {type:"canvas", id:"x"};',

    'const unsupported: ExtensionCommand = {id:"x", title:"X", capability:"files.delete"};',
    'const pathArgs: ExtensionCommand = {id:"x", title:"X", capability:"files.copySelectionToOtherPane", args:{path:"C:/outside"}};',
    'const emptyArgs: ExtensionCommand = {id:"x", title:"X", capability:"navigation.back", args:{}};',
    'const undefinedArgs: ExtensionCommand = {id:"x", title:"X", capability:"view.refresh", args:undefined};',
    'const missingMode: ExtensionCommand = {id:"x", title:"X", capability:"view.setMode"};',
    'const wrongMode: ExtensionCommand = {id:"x", title:"X", capability:"view.setMode", args:{mode:"invented"}};',
    'const wrongLayout: ExtensionCommand = {id:"x", title:"X", capability:"workspace.setLayout", args:{mode:"details"}};',
    'const foreignBinding: ExtensionPanelField = {label:"Other tasks", binding:"context.allTasks"};',
    'const executedTemplate: ExtensionTemplateResult = {template:"transfer-panel", manifest:"{}", validated:true, executed:true};',
    'const unsupportedRuntime: ExtensionManifest = {apiVersion:1, id:"x", name:"X", commands:[], runtime:"typescript"};',
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-root', type=Path, default=ROOT)
    parser.add_argument('--tsc', type=Path, required=True)
    parser.add_argument('--node', default='node')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sdk, output = args.sdk_root.absolute(), args.output.absolute()
    output.mkdir(exist_ok=False)
    expected = json.loads((ROOT / 'tools/extensions/typecheck/package.json').read_text(encoding='utf-8'))['devDependencies']['typescript']
    version = subprocess.run([args.node, str(args.tsc.absolute()), '--version'], capture_output=True, timeout=30)
    if version.returncode or version.stdout.decode('utf-8').strip() != 'Version ' + expected:
        raise RuntimeError('TypeScript compiler does not match the pinned version ' + expected)
    names = json.loads((sdk / 'examples/extensions/catalog.json').read_text(encoding='utf-8'))['templates']
    examples = [json.loads((sdk / 'examples/extensions' / name / 'manifest.json').read_text(encoding='utf-8')) for name in names]
    module = os.path.relpath(sdk / 'sdk/typescript/index', output).replace('\\', '/')
    if not module.startswith('.'):
        module = './' + module
    code = 'import type {ExtensionManifest, ExtensionCommand, ExtensionPanelField, ExtensionTemplateResult, GetExtensionTemplateArgs, ExtensionApiDescription, UiNode, ExtensionPanel, ResourceRequest, ResourceResponse, ResourceResponseFor, SortColumn, SortDirection} from ' + json.dumps(module) + ';\n'
    code += 'const examples: ExtensionManifest[] = ' + json.dumps(examples, ensure_ascii=True) + ';\n'
    code += 'const sortColumns: SortColumn[] = ["name","modified","created","accessed","type","size","path"];\n'
    code += 'const sortDirections: SortDirection[] = ["ascending","descending"];\n'
    code += 'const selectionCommands: ExtensionCommand[] = [{id:"a",title:"All",capability:"selection.selectAll"},{id:"i",title:"Invert",capability:"selection.invert"},{id:"c",title:"Clear",capability:"selection.clear"}];\n'
    code += 'const sharedControls: UiNode[] = [{type:"switch",id:"s",state:"review"},{type:"button",id:"b",command:"x",kind:"primary"},{type:"textInput",id:"i",text:"Name",placeholder:"Enter name"},{type:"progress",id:"p",kind:"ring",status:"paused",value:75}];\n'
    code += 'const machineContext: ResourceResponse<"context.get"> = {ok:true,data:{directory:"",selectionCount:0,selectionName:"",viewMode:"details",loading:false}};\n'
    code += 'const noTabContext: ResourceResponse<"context.get"> = {ok:true,data:{directory:"",selectionCount:0,selectionName:"",viewMode:null,loading:false}};\n'
    code += 'const resources: ResourceRequest[] = [{op:"context.get"},{op:"files.list",limit:64},{op:"files.stat",entry:"opaque"},{op:"operations.get",taskId:"1"},{op:"operations.cancel",taskId:"1"},{op:"settings.get"},{op:"settings.set",key:"local",value:null,expectedVersion:0},{op:"audit.list",limit:64}];\n'
    code += 'const resourceReply: ResourceResponse<"settings.get"> = {ok:true,data:{version:0,values:{label:"hello"}}};\n'
    code += 'const correlated: ResourceResponseFor<{op:"operations.cancel",taskId:"1"}> = {ok:true,data:{accepted:true,taskId:"1"}};\n'
    code += 'const failure: ResourceResponse<"files.list"> = {ok:false,error:{code:"stale_reference"}};\n'
    code += 'function consume(reply: ResourceResponse<"settings.get">): number {return reply.ok ? reply.data.version : reply.error.code.length;}\n'
    code += 'const templateArgs: GetExtensionTemplateArgs = {template:"transfer-panel"};\n'
    code += 'const templateResult: ExtensionTemplateResult = {template:templateArgs.template, manifest:JSON.stringify(examples[0]), validated:true, executed:false};\n'
    api_path = sdk / 'schemas/extensions/host-api.json'
    if api_path.is_file():
        code += 'const hostApi: ExtensionApiDescription = ' + json.dumps(json.loads(api_path.read_text(encoding='utf-8')), ensure_ascii=True) + ';\n'
    for case in NEGATIVE:
        code += '// @ts-expect-error This violates the public extension contract.\n' + case + '\n'
    fixture = output / 'sdk-contract.ts'
    fixture.write_text(code, encoding='utf-8')
    arguments = [args.node, str(args.tsc.absolute()), '--noEmit', '--strict', '--exactOptionalPropertyTypes',
                 '--target', 'ES2020', '--module', 'commonjs', '--moduleResolution', 'node', '--pretty', 'false', str(fixture)]
    result = subprocess.run(arguments, cwd=output, capture_output=True, timeout=60)
    (output / 'compiler.log').write_bytes(result.stdout + result.stderr)
    report = {'compilerVersion': expected, 'templates': names, 'positiveTemplateCount': len(examples),
              'negativeAssertionCount': len(NEGATIVE), 'hostApiCompiled': api_path.is_file(),
              'strict': True, 'exactOptionalPropertyTypes': True, 'exitCode': result.returncode}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report), flush=True)
    if result.returncode:
        print((result.stdout + result.stderr).decode('utf-8', errors='replace'), file=sys.stderr)
    return result.returncode


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    raise SystemExit(main())
