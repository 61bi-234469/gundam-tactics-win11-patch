#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
/* Reproduce gundam.exe's MIDI BGM loop: OpenAndPlayMidiTrack then PollLoopingMidiBgm */
static const char *modename(DWORD m){switch(m){case MCI_MODE_STOP:return "STOP";case MCI_MODE_PLAY:return "PLAY";case MCI_MODE_PAUSE:return "PAUSE";case MCI_MODE_NOT_READY:return "NOT_READY";case MCI_MODE_SEEK:return "SEEK";case MCI_MODE_OPEN:return "OPEN";default:return "?";}}
static LRESULT CALLBACK wndproc(HWND h,UINT m,WPARAM w,LPARAM l){ if(m==MM_MCINOTIFY){printf("%lu MM_MCINOTIFY status=%u dev=%u\n",GetTickCount(),(unsigned)w,(unsigned)l);fflush(stdout);} return DefWindowProcA(h,m,w,l);}
int main(int argc,char**argv){
  const char *path = argc>1?argv[1]:"C:\\GT\\Sound\\GMSYS_ON.MID";
  int seconds = argc>2?atoi(argv[2]):40;
  WNDCLASSA wc={0}; wc.lpfnWndProc=wndproc; wc.hInstance=GetModuleHandleA(NULL); wc.lpszClassName="mcitest"; RegisterClassA(&wc);
  HWND hwnd=CreateWindowA("mcitest","mcitest",WS_OVERLAPPEDWINDOW,0,0,100,100,NULL,NULL,wc.hInstance,NULL);
  MCI_OPEN_PARMSA op={0}; op.lpstrDeviceType="sequencer"; op.lpstrElementName=path;
  MCIERROR e=mciSendCommandA(0,MCI_OPEN,MCI_OPEN_TYPE|MCI_OPEN_ELEMENT,(DWORD_PTR)&op);
  printf("open err=%lu id=%u path=%s\n",(unsigned long)e,(unsigned)op.wDeviceID,path); if(e) return 1;
  MCIDEVICEID id=op.wDeviceID;
  MCI_STATUS_PARMS st={0}; st.dwItem=MCI_STATUS_LENGTH; mciSendCommandA(id,MCI_STATUS,MCI_STATUS_ITEM,(DWORD_PTR)&st); printf("length=%lu ms\n",(unsigned long)st.dwReturn); st.dwItem=MCI_STATUS_MODE;
  e=mciSendCommandA(id,MCI_STATUS,MCI_STATUS_ITEM,(DWORD_PTR)&st); printf("status0 err=%lu mode=%s\n",(unsigned long)e,modename(st.dwReturn));
  MCI_PLAY_PARMS pp={0}; pp.dwCallback=(DWORD_PTR)hwnd;
  e=mciSendCommandA(id,MCI_PLAY,MCI_NOTIFY,(DWORD_PTR)&pp); printf("%lu play err=%lu\n",GetTickCount(),(unsigned long)e);
  DWORD start=GetTickCount(), last=0xFFFFFFFF; int restarts=0;
  while(GetTickCount()-start < (DWORD)seconds*1000){
    MSG msg; while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageA(&msg);}
    st.dwItem=MCI_STATUS_MODE; e=mciSendCommandA(id,MCI_STATUS,MCI_STATUS_ITEM,(DWORD_PTR)&st);
    if(st.dwReturn!=last){ MCI_STATUS_PARMS ps={0}; ps.dwItem=MCI_STATUS_POSITION; mciSendCommandA(id,MCI_STATUS,MCI_STATUS_ITEM,(DWORD_PTR)&ps);
      printf("%lu mode=%s (err=%lu) pos=%lu\n",GetTickCount(),modename(st.dwReturn),(unsigned long)e,(unsigned long)ps.dwReturn); fflush(stdout); last=st.dwReturn; }
    if(st.dwReturn==MCI_MODE_PAUSE){ e=mciSendCommandA(id,MCI_RESUME,0,0); printf("resume err=%lu\n",(unsigned long)e);}
    if(st.dwReturn==MCI_MODE_STOP){
      MCI_SEEK_PARMS sk={0}; e=mciSendCommandA(id,MCI_SEEK,MCI_SEEK_TO_START,(DWORD_PTR)&sk); printf("%lu seek err=%lu\n",GetTickCount(),(unsigned long)e);
      pp.dwCallback=(DWORD_PTR)hwnd; e=mciSendCommandA(id,MCI_PLAY,MCI_NOTIFY,(DWORD_PTR)&pp); printf("%lu replay err=%lu\n",GetTickCount(),(unsigned long)e); fflush(stdout);
      restarts++; if(restarts>=3) break; last=0xFFFFFFFF; }
    Sleep(100);
  }
  printf("restarts=%d\n",restarts);
  mciSendCommandA(id,MCI_CLOSE,0,0); return 0;
}
