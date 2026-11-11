/* SPDX-License-Identifier: MIT */
static struct agxw_adapter_identity identity;
static unsigned entries=2,owned,closed,calls;
static int scenario;
static struct agx_d3dkmt_winsys {struct agx_d3dkmt_fns fns;struct agx_kmt_luid luid;agx_kmt_handle adapter;} ws;
static volatile LONG generation=7,in_flight;
static agx_kmt_status query_runtime(agxw_ddi_handle h,const struct agxw_ddi_queryadapterinfo *q){
 assert((uintptr_t)h==0x10001000 && in_flight==1 && host_lock_depth==0);
 if(scenario==1)return 1;
 memcpy(q->pPrivateDriverData,&identity,sizeof(identity));
 if(scenario==2)((struct agxw_adapter_identity*)q->pPrivateDriverData)->struct_size--;
 return 0;
}
static agx_kmt_status enumerate(struct agx_kmt_enumadapters2 *e){
 calls++;
 if(!e->pAdapters){e->NumAdapters=scenario==3?0:scenario==4?65:entries;return scenario==5?-1:0;}
 for(unsigned i=0;i<entries;i++){e->pAdapters[i]=(struct agx_kmt_adapterinfo){.hAdapter=100+i,.AdapterLuid={i+1,0}};if(scenario!=6 && scenario!=7)owned++;}
 if(scenario==17){e->pAdapters[1].hAdapter=0;owned--;}
 if(scenario==18){e->pAdapters[1].hAdapter=e->pAdapters[0].hAdapter;owned--;}
 if(scenario==6)return -1;
 if(scenario==7)e->NumAdapters=entries+1;
 return 0;
}
static agx_kmt_status query_raw(struct agx_kmt_queryadapterinfo *q){
 if(q->hAdapter==ws.adapter){if(scenario==8)return 1;memcpy(q->pPrivateDriverData,&identity,sizeof(identity));if(scenario==9)((struct agxw_adapter_identity*)q->pPrivateDriverData)->chip_id++;return 0;}
 assert(q->hAdapter>=100 && q->hAdapter<100+entries);
 unsigned index=q->hAdapter-100;
 if(q->Type==AGX_KMT_QAITYPE_ADAPTERTYPE){if(scenario==10 && index==1)return -1;((struct agx_kmt_adaptertype*)q->pPrivateDriverData)->Value=index==0||scenario>=11?SDK_RENDER_SUPPORTED:(SDK_RENDER_SUPPORTED|SDK_SOFTWARE_DEVICE);if(index==0 && scenario==19)((struct agx_kmt_adaptertype*)q->pPrivateDriverData)->Value=SDK_RENDER_SUPPORTED|SDK_POST_DEVICE;
 if(index==0 && scenario==20)((struct agx_kmt_adaptertype*)q->pPrivateDriverData)->Value=SDK_RENDER_SUPPORTED|SDK_DISPLAY_SUPPORTED;
 if(index==1 && (scenario==19||scenario==20))((struct agx_kmt_adaptertype*)q->pPrivateDriverData)->Value=SDK_RENDER_SUPPORTED|SDK_SOFTWARE_DEVICE;
 if(index==1 && scenario==21)((struct agx_kmt_adaptertype*)q->pPrivateDriverData)->Value=SDK_RENDER_SUPPORTED|SDK_POST_DEVICE;
 return 0;}
 if((scenario==0||scenario>=19)&&index==1)return -1;
 if(scenario==11 && index==1)return -1;
 memcpy(q->pPrivateDriverData,&identity,sizeof(identity));
 if(scenario==12 && index==1)((struct agxw_adapter_identity*)q->pPrivateDriverData)->magic=0;
 return 0;
}
static agx_kmt_status close_adapter(const struct agx_kmt_closeadapter *q){assert(q->hAdapter>=100 && q->hAdapter<100+entries);closed++;return scenario==14?-1:0;}
static bool bridge_adapter_matches(struct agx_d3dkmt_winsys*,const AuroraGpuBridge*);
static unsigned checks;
int main(void){
 setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
 identity=(struct agxw_adapter_identity){.magic=AGXW_ADAPTER_IDENTITY_MAGIC,.version=AGXW_ADAPTER_IDENTITY_VERSION,.struct_size=sizeof(identity),.contract_version=AGXW_CONTRACT_VERSION,.params_blob_size=AGX_D3DKMT_PARAMS_GLOBAL_SIZE,.chip_id=0x8142,.gpu_generation=17,.gpu_variant='G'};
 ws=(struct agx_d3dkmt_winsys){.adapter=0xa1,.luid={1,0},.fns={.EnumAdapters2=enumerate,.QueryAdapterInfo=query_raw,.CloseAdapter=close_adapter}};
 AuroraGpuBridge b={.struct_size=sizeof(b),.abi_version=4,.hRTAdapter=(void*)0x10001000,.hRTDevice=(void*)0x20002000,.alive=(const volatile long*)&generation,.alive_value=7,.in_flight=(volatile long*)&in_flight,.query_adapter_info=(const void*)query_runtime};
 for(scenario=0;scenario<=21;scenario++){
  owned=closed=calls=0;generation=scenario==16?8:7;ws.luid.LowPart=scenario==15?9:1;
  printf("BEGIN scenario%d\n",scenario);
  bool ok=bridge_adapter_matches(&ws,&b);
  bool expected=scenario==0||scenario==19||scenario==20;
  if(ok!=expected||owned!=closed||in_flight!=0){printf("FAIL adapter scenario%d accepted=%u owned=%u closed=%u inflight=%d\n",scenario,ok,owned,closed,in_flight);return 1;}
  checks++;printf("PASS adapter scenario%d accepted=%u handles=%u/%u\n",scenario,ok,owned,closed);
 }
 printf("adapter identity checks=%u hardware=false\n",checks);return 0;
}
