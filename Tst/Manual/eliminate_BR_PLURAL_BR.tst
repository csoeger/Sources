LIB "tst.lib"; tst_init();
ring r=0,(e,f,h,a),Dp;
matrix d[4][4];
d[1,2]=-h; d[1,3]=2*e; d[2,3]=-2*f;
def R=nc_algebra(1,d); setring R;
// this algebra is U(sl_2), tensored with K[a] over K
option(redSB);
option(redTail);
poly  p = 4*e*f+h^2-2*h - a;
// p is a central element with parameter
ideal I = e^3, f^3, h^3-4*h, p; // take this ideal
// and intersect I with the ring K[a]
ideal J = eliminate(I,e*f*h);
// if we want substitute 'a' with a value,
// it has to be a root of this polynomial
J;
// now we try to eliminate h,
// that is we intersect I with the subalgebra S,
// generated by e and f.
// But S is not closed in itself, since f*e-e*f=-h !
// the next command will definitely produce an error
eliminate(I,h);
// since a commutes with e,f,h, we can eliminate it:
eliminate(I,a);
tst_status(1);$
