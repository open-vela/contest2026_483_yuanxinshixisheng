#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "vv_k7_trng_development.h"
#include "vv_rtc_provision_i2c2.h"

#include "vv_dns_client.h"
#include "vv_native_service.h"
#include "vv_k7_platform_check.h"
#include "vv_dns_nuttx.h"
#include "vv_https_client.h"
#include "vv_https_mbedtls.h"

#ifdef K7CLOUD_MIMO_V25
#  include "mimo_v25_profile.h"
#endif

#ifdef K7CLOUD_SPEECH_ORCHESTRATOR
#  include "cloud_speech_mimo_adapter.h"
int k7cloud_capture_stream_probe(void);
int k7cloud_board_speech(const char *mode, const char *ip);
int k7cloud_gateway_probe(const char *ip);
int k7cloud_tts_control(int stop);
int k7cloud_device_once(const char *ip);
int k7cloud_chat_loop(const char *ip,unsigned rounds);
int k7cloud_device_loop(const char *ip,unsigned rounds);
int k7cloud_device_service(const char *ip);
int k7cloud_device_stop(void);
int k7cloud_photo_retry(const char *ip);
int k7cloud_report_play(const char *ip,const char *task);
#endif

#ifdef K7CLOUD_RADIO_READINESS
#  include "k7_radio_service.h"
#endif

#if defined(K7_APM21_OFFLINE_DIAG) && K7_APM21_OFFLINE_DIAG
int k7_apm21_offline_command(int argc, char **argv);
#endif
#if defined(K7_APM21_RECORDED_QUIET) && K7_APM21_RECORDED_QUIET
int k7_apm21_recorded_quiet_command(int argc, char **argv);
#endif

static void usage(void)
{
  puts("usage: k7cloud status");
#if defined(K7_APM21_OFFLINE_DIAG) && K7_APM21_OFFLINE_DIAG
  puts("       k7cloud apm-offline-check impulse|prbs (synthetic CPU only)");
  puts("       k7cloud apm-offline-status | apm-offline-dump <key> <first> <count:1..512>");
#endif
  puts("       k7cloud native-status | native-run | native-stop");
  puts("       k7cloud native-platform-check (explicit once: RTC/clock gate, no audio)");
  puts("       k7cloud native-trng-check | native-rtc-set <UTC epoch seconds>");
  puts("       k7cloud native-rtc-init <UTC epoch seconds> (explicitly start stopped RTC)");
  puts("       k7cloud native-rtc-raw (read only registers 00..08, requires prepared RTC)");
#ifdef K7CLOUD_MIMO_V25
  puts("       k7cloud mimo-profile <regional-host>");
#endif
#ifdef K7CLOUD_SPEECH_ORCHESTRATOR
  puts("       k7cloud speech-status");
  puts("       k7cloud gateway-check <gateway-ip> (TCP only, no audio)");
  puts("       k7cloud chat-loop <gateway-ip> <rounds:1..300>");
  puts("       k7cloud device-service <gateway-ip> (until stop or error)");
  puts("       k7cloud capture-stream-check");
#endif
#ifdef K7CLOUD_RADIO_READINESS
  puts("       k7cloud radio-status");
#endif
}

#ifdef K7CLOUD_RADIO_READINESS
/* Read-only diagnostics: do not apply a connected transition to a temporary
 * orchestrator, which would replay the connected prompt on every query. */
static int radio_status(void)
{
  struct k7radio_link_snapshot snapshot;
  int rc = k7radio_get_link_snapshot(&snapshot);

  if (rc != 0)
    {
      printf("K7CLOUD radio_snapshot=unavailable error=%d cloud_ready=0\n", rc);
      return 1;
    }

  printf("K7CLOUD radio_generation=%" PRIu64 " wifi=%u ipv4_ready=%u\n",
         snapshot.generation, (unsigned)snapshot.wifi_connected,
         (unsigned)snapshot.ipv4_ready);
  printf("K7CLOUD ipv4=%u.%u.%u.%u\n",
         (unsigned)snapshot.ipv4[0], (unsigned)snapshot.ipv4[1],
         (unsigned)snapshot.ipv4[2], (unsigned)snapshot.ipv4[3]);
  puts("K7CLOUD platform_gates=unverified cloud_ready=0 network_request=disabled");
  return 0;
}
#endif

static int link_status(void)
{
  struct vv_dns_result dns_result;
  struct vv_https_response response;

  /* Exercise only the public argument-validation paths.  These calls keep the
   * production adapter sections in the ELF without opening a socket, seeding a
   * DRBG or accepting any endpoint or credential. */
  if (strcmp(vv_dns_status_name(VV_DNS_OK), "ok") != 0 ||
      vv_dns_resolve_ipv4(NULL, NULL, NULL, &dns_result) !=
        VV_DNS_INVALID_ARGUMENT ||
      vv_dns_nuttx_backend_init(NULL, NULL) != -EINVAL ||
      strcmp(vv_https_strerror(VV_HTTPS_OK), "ok") != 0 ||
      vv_https_perform(NULL, NULL, NULL, NULL, NULL, &response) !=
        VV_HTTPS_EINVAL ||
      vv_https_mbedtls_create(NULL) != NULL)
    {
      puts("K7CLOUD link_selftest=failed");
      return 1;
    }

  puts("K7CLOUD DNS=linked HTTPS=linked");
  puts("K7CLOUD link_selftest=passed");
  puts("K7CLOUD entropy=unverified wall_time=unverified ca=unset");
  puts("K7CLOUD network_request=disabled");
#ifdef K7CLOUD_MIMO_V25
  puts("K7CLOUD mimo_v25=linked token=unset");
#else
  puts("K7CLOUD mimo_v25=disabled");
#endif
#ifdef K7CLOUD_SPEECH_ORCHESTRATOR
  puts("K7CLOUD speech_orchestrator=linked runtime=not_configured");
#else
  puts("K7CLOUD speech_orchestrator=disabled");
#endif
  return 0;
}

#ifdef K7CLOUD_MIMO_V25
static int profile_status(const char *host)
{
  struct mimo_cloud_protocol protocol;

  if (mimo_v25_profile_init(&protocol, host) != MIMO_CLOUD_OK ||
      strcmp(protocol.asr_model, "mimo-v2.5-asr") != 0 ||
      strcmp(protocol.tts_model, "mimo-v2.5-tts") != 0 ||
      protocol.port != 443)
    {
      puts("K7CLOUD mimo_profile=invalid");
      return 1;
    }

  /* Do not echo the caller's regional host.  This command validates the
   * profile only and deliberately accepts no token or network operation. */
  puts("K7CLOUD mimo_profile=valid asr=mimo-v2.5-asr tts=mimo-v2.5-tts");
  puts("K7CLOUD token=unset network_request=disabled");
  return 0;
}
#endif

#ifdef K7CLOUD_SPEECH_ORCHESTRATOR
static int speech_status(void)
{
  struct cloud_speech_ops ops;
  struct cloud_speech_orchestrator orchestrator;
  unsigned char wav_scratch[46];
  int16_t pcm_scratch[1];

  if (cloud_speech_mimo_adapter_ops(&ops) != CLOUD_SPEECH_OK ||
      ops.play_fixed_prompt == NULL || ops.cloud_asr_wav == NULL ||
      ops.cloud_tts_wav == NULL || ops.play_pcm16_16k == NULL ||
      cloud_speech_init(&orchestrator, &ops, NULL, wav_scratch,
                        sizeof(wav_scratch), pcm_scratch, 1) !=
        CLOUD_SPEECH_OK || cloud_speech_is_ready(&orchestrator))
    {
      puts("K7CLOUD speech_adapter=failed");
      return 1;
    }

  puts("K7CLOUD speech_adapter=linked audio=pcm16le/16000/mono");
  puts("K7CLOUD readiness=0x00 required=0xff cloud_ready=0");
  puts("K7CLOUD runtime=not_configured network_request=disabled");
  return 0;
}
#endif

static int native_status(void)
{
 struct vv_native_service_status s;
 int rc=vv_native_service_status(&s);if(rc)return 1;
 printf("K7CLOUD native configured=%u running=%u stopping=%u last_error=%d\n",(unsigned)s.configured,(unsigned)s.running,(unsigned)s.stopping,s.last_error);
 if(s.report_available)printf("K7CLOUD native report_error=%d assessment_outcome=%d upload_attempted=%u required_views=%u played_segments=%lu\n",s.report_error,(int)s.assessment.outcome,(unsigned)s.assessment.upload_attempted,s.assessment.required_views,(unsigned long)s.audio.segments_played);
 static const struct {unsigned bit;const char *name;} fields[]={
 {VV_NATIVE_CONFIG,"platform_registration"},{VV_NATIVE_NETWORK,"network"},
 {VV_NATIVE_TIME,"trusted_time"},{VV_NATIVE_CA,"ca"},{VV_NATIVE_ENTROPY,"entropy"},
 {VV_NATIVE_ENDPOINT,"endpoint"},{VV_NATIVE_AUTH,"authentication"},
 {VV_NATIVE_PREVIEW,"preview_pipeline"},{VV_NATIVE_OWNER,"exclusive_owner"},
 {VV_NATIVE_METADATA,"consent_and_request_metadata"},{VV_NATIVE_BUSINESS_AUTH,"business_login_photo_only"}};
 for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++)
  if(s.missing&fields[i].bit)printf("K7CLOUD native missing=%s\n",fields[i].name);
 if(!s.configured)puts("K7CLOUD native required=network,trusted_time,ca,entropy,endpoint,authentication,preview_pipeline,exclusive_owner,consent_and_request_metadata");
 return 0;
}

#if defined(K7_APM21_FIXED_PHYSICAL) && K7_APM21_FIXED_PHYSICAL
int k7_apm21_fixed_physical_command(int argc,char **argv);
#endif
int main(int argc, char **argv)
{
#if defined(K7_APM21_FIXED_PHYSICAL) && K7_APM21_FIXED_PHYSICAL
  if (argc>=2 && !strncmp(argv[1],"apm-fixed-",10))
    return k7_apm21_fixed_physical_command(argc,argv);
#endif
#if defined(K7_APM21_RECORDED_QUIET) && K7_APM21_RECORDED_QUIET
  if (argc==2 && !strcmp(argv[1],"apm-recorded-quiet"))
    return k7_apm21_recorded_quiet_command(argc,argv);
#endif
#if defined(K7_APM21_OFFLINE_DIAG) && K7_APM21_OFFLINE_DIAG
  if (argc>=2 && (!strcmp(argv[1],"apm-offline-check") ||
      !strcmp(argv[1],"apm-offline-status") || !strcmp(argv[1],"apm-offline-dump")))
    return k7_apm21_offline_command(argc,argv);
#endif
#ifdef CONFIG_VELAVISION_TRNG_NS_DEVELOPMENT
  if(argc==2 && !strcmp(argv[1],"native-trng-check")){
    int rc=vv_k7_trng_development_probe();
    printf("K7CLOUD trng_probe rc=%d state=%d random_logged=0\n",rc,vv_k7_trng_development_state());
    return rc?1:0;
  }
#endif
#ifdef CONFIG_VELAVISION_RTC_I2C2_PLATFORM
  if(argc==2 && !strcmp(argv[1],"native-rtc-raw")){
    struct vv_rtc_provision_ops ops;struct vv_rtc_reader reader={0};
    struct timespec now;uint8_t raw[9]={0};unsigned count=0;
    int rc=vv_rtc_i2c2_platform_provision_ops(&ops);
    if(!rc && clock_gettime(CLOCK_MONOTONIC,&now))rc=-EIO;
    if(!rc){reader.ops=ops.reader;rc=vv_rtc_reader_raw(&reader,(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000+1000,raw,&count);}
    printf("K7CLOUD rtc_raw rc=%d count=%u bytes=",rc,count);
    for(unsigned i=0;i<count;i++)printf("%02x%s",raw[i],i+1==count?"":" ");
    putchar('\n');fflush(stdout);usleep(5000);return rc?1:0;
  }
  if(argc==3 && (!strcmp(argv[1],"native-rtc-set") || !strcmp(argv[1],"native-rtc-init"))){
    static struct vv_rtc_provision provision;
    static volatile int owner;
    struct vv_rtc_provision_result result={0};struct timespec now;
    uint64_t epoch;int rc=vv_rtc_provision_parse_epoch(argv[2],&epoch);
    if(rc)return 1;
    if(__sync_lock_test_and_set(&owner,1))return 1;
    rc=vv_rtc_i2c2_platform_provision_ops(&provision.ops);
    if(!rc && clock_gettime(CLOCK_MONOTONIC,&now))rc=-EIO;
    if(!rc){
      uint64_t deadline=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000+1000;
      rc=!strcmp(argv[1],"native-rtc-init")?
        vv_rtc_provision_initialize_cli(&provision,true,argv[2],deadline,&result):
        vv_rtc_provision_cli(&provision,true,argv[2],deadline,&result);
    }
    printf("K7CLOUD rtc_set rc=%d valid=%d utc=%" PRIu64 " vl_before=%d\n",rc,result.readback_valid,result.observed_epoch,result.initial_vl);
    fflush(stdout);usleep(5000);
    printf("K7CLOUD rtc_preflight ctl_valid=%d ctl=%02x sec_valid=%d sec=%02x blocked=%02x\n",
      result.initial_control1_valid,result.initial_control1,result.initial_seconds_valid,
      result.initial_seconds,result.control_blocked);
    fflush(stdout);usleep(5000);
    printf("K7CLOUD rtc_restore rc=%d cleanup=%d restored=%d\n",result.restore_rc,result.cleanup_rc,result.restored);
    fflush(stdout);usleep(5000);
    printf("K7CLOUD rtc_init requested=%d running=%d rollback=%d rollback_rc=%d\n",
      result.initialize_requested,result.running_verified,result.rollback_attempted,result.rollback_rc);
    fflush(stdout);usleep(5000);
    printf("K7CLOUD rtc_write attempted=%d acked=%d operation_rc=%d\n",result.write_attempted,result.write_acked,result.operation_rc);
    fflush(stdout);usleep(5000);
    for(unsigned pass=0;pass<2;pass++){
      printf("K7CLOUD rtc_readback pass=%u count=%u bytes=",pass,result.readback_count[pass]);
      for(unsigned i=0;i<result.readback_count[pass];i++)printf("%02x%s",result.readback[pass][i],i+1==result.readback_count[pass]?"":" ");
      putchar('\n');fflush(stdout);usleep(5000);
    }
    __sync_lock_release(&owner);return rc?1:0;
  }
#endif
  if(argc==2 && !strcmp(argv[1],"native-platform-check"))return vv_k7_platform_check();
  if(argc==2 && !strcmp(argv[1],"native-status"))return native_status();
  if(argc==2 && !strcmp(argv[1],"native-run")){
    int rc=vv_native_service_run();printf("K7CLOUD native run_result=%d\n",rc);
    if(rc)native_status();
    return rc?1:0;
  }
  if(argc==2 && !strcmp(argv[1],"native-stop")){
    int rc=vv_native_service_stop();printf("K7CLOUD native stop_requested result=%d\n",rc);
    return rc?1:0;
  }

#ifdef K7CLOUD_SPEECH_ORCHESTRATOR
  if (argc == 3 && !strcmp(argv[1], "gateway-check"))
    return k7cloud_gateway_probe(argv[2]) ? 1 : 0;
  if(argc==4 && !strcmp(argv[1],"report-play"))return k7cloud_report_play(argv[2],argv[3])?1:0;
  if(argc==3 && !strcmp(argv[1],"photo-retry"))return k7cloud_photo_retry(argv[2])?1:0;
  if(argc==2 && !strcmp(argv[1],"device-stop"))return k7cloud_device_stop()?1:0;
  if(argc==3 && !strcmp(argv[1],"device-service"))
    {
#ifdef K7CLOUD_RADIO_READINESS
      struct k7radio_link_snapshot link;
      int rc=k7radio_get_link_snapshot(&link);
      if(rc || !link.wifi_connected || !link.ipv4_ready)
        {puts("K7CLOUD device service_rejected=network_not_ready");return 1;}
      return k7cloud_device_service(argv[2])?1:0;
#else
      puts("K7CLOUD device service_rejected=radio_unavailable");return 1;
#endif
    }
  if(argc==4 && (!strcmp(argv[1],"device-loop") || !strcmp(argv[1],"chat-loop")))
    {
      char *end;errno=0;unsigned long rounds=strtoul(argv[3],&end,10);
      if(errno||!argv[3][0]||*end||rounds<1||rounds>300)return 1;
      return (!strcmp(argv[1],"chat-loop")?k7cloud_chat_loop(argv[2],(unsigned)rounds):k7cloud_device_loop(argv[2],(unsigned)rounds))?1:0;
    }
  if(argc==3 && !strcmp(argv[1],"device-once"))
    return k7cloud_device_once(argv[2]) ? 1 : 0;
  if (argc == 2 && !strcmp(argv[1], "tts-stop")) return k7cloud_tts_control(1);
  if (argc == 2 && !strcmp(argv[1], "tts-state")) return k7cloud_tts_control(0);
  if (argc == 3 && (!strcmp(argv[1], "asr-live") || !strcmp(argv[1], "tts-test")))
    return k7cloud_board_speech(argv[1], argv[2]) ? 1 : 0;
  if (argc == 4 && !strcmp(argv[1], "tts-prompt"))
    {
      /* Key only: the table supplies text. Never pass raw ASR into a shell. */
      if (!strcmp(argv[3],"asr-live") || !strcmp(argv[3],"tts-test")) return 1;
      return k7cloud_board_speech(argv[3], argv[2]) ? 1 : 0;
    }
  if (argc == 2 && strcmp(argv[1], "capture-stream-check") == 0)
    return k7cloud_capture_stream_probe() ? 1 : 0;
#endif
#ifdef K7CLOUD_RADIO_READINESS
  if (argc == 2 && strcmp(argv[1], "radio-status") == 0)
    {
      return radio_status();
    }
#endif
  if (argc == 2 && strcmp(argv[1], "status") == 0)
    {
      return link_status();
    }

#ifdef K7CLOUD_MIMO_V25
  if (argc == 3 && strcmp(argv[1], "mimo-profile") == 0)
    {
      return profile_status(argv[2]);
    }
#endif

#ifdef K7CLOUD_SPEECH_ORCHESTRATOR
  if (argc == 2 && strcmp(argv[1], "speech-status") == 0)
    {
      return speech_status();
    }
#endif

  usage();
  return 2;
}
