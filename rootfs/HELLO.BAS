REM HELLO.BAS - the first bdos program. EDIT it, press F5 to run it.
PRINT "Hello from bdos BASIC!"
INPUT "What's your name? " name$
PRINT "Nice to meet you, "; name$; "."
FOR i% = 1 TO 5
  PRINT STRING$(i%, "*")
NEXT
