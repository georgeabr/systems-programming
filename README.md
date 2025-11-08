# Systems programming
Systems programming - software for Windows NT 3.51 in C++.  
You need Visual C++ 4.2 to compile these programs.

### [w32time](/w32time/w32time.cpp)
It gets time from an NTP server and optionally syncs it.
```
$ w32time.exe pool.ntp.org -set
Server pool.ntp.org UTC time: 2025-11-08 12:26:37
System time updated.

$ w32time.exe pool.ntp.org
Server pool.ntp.org UTC time: 2025-11-08 12:26:37

```

### [taskman](/taskman/task-man.cpp)
A task manager that shows open applications to which you can switch and end.  
You can also run new applications.  
![Task Manager screenshot](taskman-screenshot1.png?raw=true "Optional Title")

### [task-inter](/task-inter/task-inter.cpp)
A global hotkey interceptor for NT. Replace the `WinExec` program path with your own.  
Default key is `Shift+Esc` to activate and run your program.  
Useful if we want to hook our own task manager for NT.  

### [taskbar](/taskbar/taskbar.cpp)
A task bar at the bottom of the screen that shows open applications to which you can switch.  
It has icons for visual guidance.  
You can double click at the end of the taskbar to exit it.  
![Task bar screenshot](taskbar-screenshot1.png?raw=true "Optional Title")
