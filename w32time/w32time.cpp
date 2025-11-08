// w32time.cpp
// SNTP client for Windows NT 3.51
// Build as Win32 Console Application with VC++ 4.0/4.2

#include <windows.h>
#include <winsock.h>
#include <stdio.h>

#pragma comment(lib,"wsock32.lib")

#define NTP_PORT 123
#define NTP_PACKET_SIZE 48

const unsigned __int64 FILETIME_TICKS_PER_SEC = 10000000i64;
const unsigned __int64 FILETIME_UNIX_EPOCH    = 11644473600i64;
const unsigned __int64 NTP_EPOCH_UNIX         = 2208988800i64;

typedef struct _NTP_PACKET {
    unsigned char li_vn_mode;
    unsigned char stratum;
    unsigned char poll;
    signed char  precision;
    unsigned long rootDelay;
    unsigned long rootDispersion;
    unsigned long refId;
    unsigned long refTS_sec;
    unsigned long refTS_frac;
    unsigned long origTS_sec;
    unsigned long origTS_frac;
    unsigned long rxTS_sec;
    unsigned long rxTS_frac;
    unsigned long txTS_sec;
    unsigned long txTS_frac;
} NTP_PACKET;

static unsigned long be32_read(const unsigned long be) {
    const unsigned char *p = (const unsigned char *)&be;
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
    ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static BOOL EnableSetTimePrivilege(void) {
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&hToken))
        return FALSE;
    if(!LookupPrivilegeValueA(NULL,"SeSystemtimePrivilege",&luid)) {
        CloseHandle(hToken);
        return FALSE;
    }
    tp.PrivilegeCount=1;
    tp.Privileges[0].Luid=luid;
    tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hToken,FALSE,&tp,sizeof(tp),NULL,NULL);
    CloseHandle(hToken);
    return (GetLastError()==ERROR_SUCCESS);
}

/* Convert NTP timestamp to SYSTEMTIME (UTC) */
static BOOL NtpToSystemTime(unsigned long sec_be, unsigned long frac_be, SYSTEMTIME *outSt) {
    unsigned long sec  = be32_read(sec_be);
    unsigned long frac = be32_read(frac_be);

    // Convert NTP seconds (since 1900) -> Unix (1970) -> FileTime (1601)
    unsigned __int64 unix_seconds = (unsigned __int64)sec - NTP_EPOCH_UNIX;
    unsigned __int64 file_seconds = unix_seconds + FILETIME_UNIX_EPOCH;
    unsigned __int64 file_ticks   = file_seconds * FILETIME_TICKS_PER_SEC;
    unsigned __int64 frac_ticks   = ((unsigned __int64)frac * FILETIME_TICKS_PER_SEC) / 4294967296ui64;

    ULARGE_INTEGER uli;
    uli.QuadPart = file_ticks + frac_ticks;

    FILETIME ft;
    ft.dwLowDateTime  = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;

    return FileTimeToSystemTime(&ft,outSt);
}

int main(int argc,char**argv) {
    if(argc<2) {
        printf("Usage: %s <server> [-set]\n",argv[0]);
        return 1;
    }
    const char* server=argv[1];
    BOOL doSet=(argc>2 && !_stricmp(argv[2],"-set"));

    WSADATA wsa;
    if(WSAStartup(MAKEWORD(1,1),&wsa)!=0) { printf("WSAStartup failed\n"); return 1; }
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(s==INVALID_SOCKET) { printf("socket failed\n"); return 1; }
    struct hostent *he=gethostbyname(server);
    if(!he) { printf("DNS failed\n"); return 1; }
    struct sockaddr_in addr;
    ZeroMemory(&addr,sizeof(addr));
    addr.sin_family=AF_INET;
    addr.sin_port=htons(NTP_PORT);
    memcpy(&addr.sin_addr,he->h_addr,sizeof(struct in_addr));

    NTP_PACKET req;
    ZeroMemory(&req,sizeof(req));
    req.li_vn_mode=0x1B; // LI=0, VN=3, Mode=3 (client)

    sendto(s,(const char*)&req,sizeof(req),0,(struct sockaddr*)&addr,sizeof(addr));

    struct sockaddr_in from; int fromlen=sizeof(from);
    NTP_PACKET resp;
    int recvd=recvfrom(s,(char*)&resp,sizeof(resp),0,(struct sockaddr*)&from,&fromlen);
    closesocket(s);
    WSACleanup();

    if(recvd<(int)NTP_PACKET_SIZE) { printf("short response\n"); return 1; }

    SYSTEMTIME stSrv;
    if(!NtpToSystemTime(resp.txTS_sec,resp.txTS_frac,&stSrv)) {
        printf("Conversion failed\n");
        return 1;
    }

    printf("Server %s UTC time: %04u-%02u-%02u %02u:%02u:%02u\n",
           server,stSrv.wYear,stSrv.wMonth,stSrv.wDay,
           stSrv.wHour,stSrv.wMinute,stSrv.wSecond);

    if(doSet) {
        if(!EnableSetTimePrivilege()) {
            printf("Privilege not enabled (%lu)\n",GetLastError());
            return 1;
        }
        if(!SetSystemTime(&stSrv)) {
            printf("SetSystemTime failed (%lu)\n",GetLastError());
            return 1;
        }
        printf("System time updated.\n");
    }
    return 0;
}
