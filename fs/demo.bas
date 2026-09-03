10 rem demo basic pe myos
20 print "tabla lui 7:"
30 for i = 1 to 10
40 print "7 x "; i; " = "; 7 * i
50 next
60 print "un numar norocos: "; rnd(100)
70 if rnd(2) = 0 then print "banul zice: cap"
80 if 1 < 2 then goto 100
90 print "aici nu se ajunge niciodata"
100 gosub 200
110 print "gata!"
120 end
200 print "  (salut din subrutina - gosub/return!)"
210 return
