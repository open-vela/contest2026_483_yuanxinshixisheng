/* Explicit, bounded native authentication diagnostic; no password argv/history.
 * Console echo is disabled before READY and restored on every exit path. */
#include <termios.h>
#include <poll.h>
static int fw_wifi_auth_console(void)
{
  struct termios saved,quiet;char password[64]={0};int ret=-EINVAL;
  if(!g_bt_host_mode || tcgetattr(0,&saved))return -ENODEV;
  quiet=saved;quiet.c_lflag&=~(ECHO|ICANON);quiet.c_cc[VMIN]=0;quiet.c_cc[VTIME]=1;
  if(tcsetattr(0,TCSANOW,&quiet))return -EIO;
  puts("WIFI PASSWORD_INPUT_READY max=63 timeout=15s echo=off");fflush(stdout);
  size_t used=0;uint64_t deadline=now_us()+15000000;bool complete=false;
  while(now_us()<deadline)
    {
      struct pollfd p={.fd=0,.events=POLLIN};if(poll(&p,1,100)<=0)continue;
      char c;if(read(0,&c,1)!=1)continue;
      if(c=='\n' || c=='\r'){complete=true;break;}
      if(c<32 || c>126 || used>=63){ret=-EINVAL;goto done;}
      password[used++]=c;
    }
  if(!complete){ret=-ETIMEDOUT;goto done;}
  if(used<8)goto done;
  ret=nxmutex_lock(&g_wifi_operation);
  if(!ret){ret=fw_wifi_join_run(true,password);nxmutex_unlock(&g_wifi_operation);}
done:
  skw_wpa_wipe(password,sizeof(password));tcflush(0,TCIFLUSH);
  if(tcsetattr(0,TCSANOW,&saved) && !ret)ret=-EIO;
  printf("WIFI authentication diagnostic finished ret=%d; password erased\n",ret);
  return ret;
}
