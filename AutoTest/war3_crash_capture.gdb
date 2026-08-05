set pagination off
set confirm off
set print thread-events off
set print pretty on
handle SIGSEGV stop print pass

echo \n=== continue until access violation ===\n
continue

echo \n=== inferior status ===\n
info program

echo \n=== exception thread registers ===\n
info registers

echo \n=== exception instruction window ===\n
x/32i $pc-32

echo \n=== exception stack words ===\n
x/128wx $esp

echo \n=== suspected container/heap words from EBX ===\n
x/48wx $ebx

echo \n=== loaded module ranges ===\n
info sharedlibrary

echo \n=== all thread backtraces ===\n
thread apply all bt

echo \n=== detach and let the product crash handler finish ===\n
detach
quit
