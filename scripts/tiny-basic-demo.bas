10 PRINT "TG11 Tiny BASIC v4 demo";
20 PRINT " (arrays + funcs)"
30 DIM A(5)
40 LET NAME$ = "friend"
50 INPUT "Your name > "; NAME$
60 PRINT "Hello, ", NAME$
70 LET SUM = 0
80 FOR I = 0 TO 5
90 LET A(I) = I * I
100 ADD SUM A(I)
110 NEXT I
120 PRINT "SUM="; SPC(1); SUM
130 PRINT "LEN(name)=", LEN(NAME$), " ASC(first)=", ASC(NAME$)
140 PRINT "VAL(\"123\")=", VAL("123"), " ABS(-9)=", ABS(-9)
150 INPUT "Pick mode (1..3) > "; MODE
160 ON MODE GOSUB 300,320,340
170 RESTORE
180 READ X, Y, LABEL$
190 PRINT TAB(2), "DATA:", X, Y, LABEL$
200 PRINT "RND(10)=", RND(10), " CHR$(33)=", CHR$(33)
210 PRINT "STR$(SUM)=", STR$(SUM)
220 PRINT "Done"
230 END
300 PRINT "Mode 1";
310 PRINT " selected"
315 RETURN
320 PRINT "Mode 2 selected"
330 RETURN
340 PRINT "Mode 3 selected"
350 RETURN
900 DATA 7, 42, "synthwave"
