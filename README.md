# Systems programming
Systems programming - software for Windows NT 3.51 in C++.  
You need Visual C++ 4.2 to compile these programs.

### [w32time](https://github.com/georgeabr/systems-programming/blob/main/w32time/w32time.cpp)
It gets time from an NTP server and optionally syncs it.
```
$ w32time.exe pool.ntp.org -set
Server pool.ntp.org UTC time: 2025-11-08 12:26:37
System time updated.

$ w32time.exe pool.ntp.org
Server pool.ntp.org UTC time: 2025-11-08 12:26:37

```
