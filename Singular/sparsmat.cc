/****************************************
*  Computer Algebra System SINGULAR     *
****************************************/
/* $Id: sparsmat.cc,v 1.23 2000-02-02 18:03:21 Singular Exp $ */

/*
* ABSTRACT: operations with sparse matrices (bareiss, ...)
*/

#include "mod2.h"
#include "structs.h"
#include "tok.h"
#include "febase.h"
#include "structs.h"
#include "intvec.h"
#include "lists.h"
#include "ring.h"
#include "polys.h"
#include "ipid.h"
#include "ideals.h"
#include "numbers.h"
#include "longrat.h"
#include "sparsmat.h"
#include "prCopy.h"

/* ----------------- macros ------------------ */
/* #define OLD_DIV */
#ifdef OLD_DIV
#define SM_MULT(A,B,C) smMult(A,B)
#define SM_DIV smPolyDiv
#else
#define SM_MULT smMultDiv
#define SM_DIV smSpecialPolyDiv
#endif
/* ----------------- general definitions ------------------ */
/* in structs.h
typedef struct smprec sm_prec;
typedef sm_prec * smpoly;
struct smprec{
  smpoly n;            // the next element
  int pos;             // position
  int e;               // level
  poly m;              // the element
  float f;             // complexity of the element
};
*/
/* declare internal 'C' stuff */
static void smExactPolyDiv(poly, poly);
static BOOLEAN smIsNegQuot(poly, const poly, const poly);
static poly smEMult(poly, const poly);
static BOOLEAN smCheckLead(const poly, const poly);
static poly smDMult(poly, const poly);
static void smPolyDivN(poly a, const number);
static BOOLEAN smSmaller(poly, poly);
static void smCombineChain(poly *, poly);
static void smFindRef(poly *, poly *, poly);

static void smElemDelete(smpoly *);
static smpoly smElemCopy(smpoly);
static float smPolyWeight(smpoly);
static smpoly smPoly2Smpoly(poly);
static poly smSmpoly2Poly(smpoly);
static BOOLEAN smHaveDenom(poly);
static number smCleardenom(ideal);

/* class for sparse matrix:
*      3 parts of matrix during the algorithm
*      m_act[cols][pos(rows)] => m_row[rows][pos(cols)] => m_res[cols][pos(rows)]
*      input                     pivotcols as rows         result
*      pivot                     like a stack              from pivot and pivotcols
*      elimination                                         rows reordered according
*                                                          to pivot choise
*                                                          stored in perm
*      a step is as follows
*      - search of pivot (smPivot)
*      - swap pivot column and last column (smSwapC)
*        select pivotrow to piv and red (smSelectPR)
*        consider sign
*      - elimination (smInitElim, sm0Elim, sm1Elim)
*        clear zero column as result of elimination (smZeroElim)
*      - tranfer from
*        piv and m_row to m_res (smRowToCol)
*        m_act to m_row (smColToRow)
*/
class sparse_mat{
private:
  int nrows, ncols;    // dimension of the problem
  int sign;            // for determinant (start: 1)
  int act;             // number of unreduced columns (start: ncols)
  int crd;             // number of reduced columns (start: 0)
  int tored;           // border for rows to reduce
  int inred;           // unreducable part
  int rpiv, cpiv;      // position of the pivot
  int normalize;       // Normalization flag
  Exponent_t *perm;    // permutation of rows
  float wpoints;       // weight of all points
  float *wrw, *wcl;    // weights of rows and columns
  smpoly * m_act;      // unreduced columns
  smpoly * m_res;      // reduced columns (result)
  smpoly * m_row;      // reduced part of rows
  smpoly red;          // row to reduce
  smpoly piv, oldpiv;  // pivot and previous pivot
  smpoly dumm;         // allocated dummy
  void smColToRow();
  void smRowToCol();
  void smFinalMult();
  void smSparseHomog();
  void smRealPivot();
  void smWeights();
  void smPivot();
  void smNewWeights();
  void smNewPivot();
  void smZeroElim();
  void smToredElim();
  void smCopToRes();
  void smSelectPR();
  void smElim();
  void sm1Elim();
  void smHElim();
  void smMultCol();
  poly smMultPoly(smpoly);
  void smActDel();
  void smColDel();
  void smPivDel();
  void smSign();
  void smInitPerm();
  int smCheckNormalize();
  void smNormalize();
public:
  sparse_mat(ideal);
  ~sparse_mat();
  int smGetSign() { return sign; }
  smpoly * smGetAct() { return m_act; }
  int smGetRed() { return tored; }
  ideal smRes2Mod();
  poly smDet();
  void smBareiss(int, int);
  void smNewBareiss(int, int);
  void smToIntvec(intvec *);
};

/* ----------------- ops with rings ------------------ */
ideal smRingCopy(ideal I, ring *ri, sip_sring &tmpR)
{
  ring origR =NULL;
  ideal II;
  if (currRing->order[0]!=ringorder_c)
  {
    origR =currRing;
    tmpR=*origR;
    int *ord=(int*)Alloc0(3*sizeof(int));
    int *block0=(int*)Alloc(3*sizeof(int));
    int *block1=(int*)Alloc(3*sizeof(int));
    ord[0]=ringorder_c;
    ord[1]=ringorder_dp;
    tmpR.order=ord;
    block0[1]=1;
    tmpR.block0=block0;
    block1[1]=tmpR.N;
    tmpR.block1=block1;
    rComplete(&tmpR,1);
    rChangeCurrRing(&tmpR,TRUE);
    // fetch data from the old ring
    II=idInit(IDELEMS(I),I->rank);
    int k;
    for (k=0;k<IDELEMS(I);k++) II->m[k] = prCopyR( I->m[k], origR);
  }
  else
  {
    II=idCopy(I);
  }
  *ri = origR;
  return II;
}

void smRingClean(ring origR, ip_sring &tmpR)
{
  rChangeCurrRing(origR,TRUE);
  rUnComplete(&tmpR);
  Free((ADDRESS)tmpR.order,3*sizeof(int));
  Free((ADDRESS)tmpR.block0,3*sizeof(int));
  Free((ADDRESS)tmpR.block1,3*sizeof(int));
}

/* ----------------- basics (used from 'C') ------------------ */
/*2
*returns the determinant of the module I;
*uses Bareiss algorithm
*/
poly smCallDet(ideal I)
{
  if (I->ncols != I->rank)
  {
    Werror("det of %d x %d module (matrix)",I->rank,I->ncols);
    return NULL;
  }
  int r=idRankFreeModule(I);
  if (I->ncols != r) // some 0-lines at the end
  {
    return NULL;
  }
  number diag,h=nInit(1);
  poly res,save;
  ring origR;
  sip_sring tmpR;
  sparse_mat *det;
  ideal II=smRingCopy(I,&origR,tmpR);

  diag = smCleardenom(II);
  det = new sparse_mat(II);
  idDelete(&II);
  if (det->smGetAct() == NULL)
  {
    delete det;
    if (origR!=NULL) smRingClean(origR,tmpR);
    return NULL;
  }
  res=det->smDet();
  if(det->smGetSign()<0) res=pNeg(res);
  delete det;
  if (origR!=NULL)
  {
    rChangeCurrRing(origR,TRUE);
    save = res;
    res = prCopyR( save, &tmpR);
    rChangeCurrRing(&tmpR,FALSE);
    pDelete(&save);
    smRingClean(origR,tmpR);
  }
  if (nEqual(diag,h) == FALSE)
  {
    pMultN(res,diag);
    pNormalize(res);
  }
  nDelete(&diag);
  nDelete(&h);
  return res;
}

lists smCallBareiss(ideal I, int x, int y)
{
  ring origR;
  sip_sring tmpR;
  lists res=(lists)AllocSizeOf(slists);
  ideal II = smRingCopy(I,&origR,tmpR);
  sparse_mat *bareiss = new sparse_mat(II);
  ideal mm=II;
  intvec *v;
  ideal m;

  if (bareiss->smGetAct() == NULL)
  {
    delete bareiss;
    if (origR!=NULL) smRingClean(origR,tmpR);
    v=NewIntvec2(1,pVariables);
  }
  else
  {
    idDelete(&II);
    bareiss->smBareiss(x, y);
    m = bareiss->smRes2Mod();
    v = NewIntvec1(bareiss->smGetRed());
    bareiss->smToIntvec(v);
    delete bareiss;
    if (origR!=NULL)
    {
      rChangeCurrRing(origR,TRUE);
      mm=idInit(IDELEMS(m),m->rank);
      int k;
      for (k=0;k<IDELEMS(m);k++) mm->m[k] = prCopyR( m->m[k], &tmpR);
      rChangeCurrRing(&tmpR,FALSE);
      idDelete(&m);
      smRingClean(origR,tmpR);
    }
    else
    {
      mm=m;
    }
  }
  res->Init(2);
  res->m[0].rtyp=MODUL_CMD;
  res->m[0].data=(void *)mm;
  res->m[1].rtyp=INTVEC_CMD;
  res->m[1].data=(void *)v;
  return res;
}

lists smCallNewBareiss(ideal I, int x, int y)
{
  ring origR;
  sip_sring tmpR;
  lists res=(lists)AllocSizeOf(slists);
  ideal II=smRingCopy(I,&origR,tmpR);
  sparse_mat *bareiss = new sparse_mat(II);
  ideal mm=II;
  intvec *v;
  ideal m;

  if (bareiss->smGetAct() == NULL)
  {
    delete bareiss;
    if (origR!=NULL) smRingClean(origR,tmpR);
    v=NewIntvec2(1,pVariables);
  }
  else
  {
    idDelete(&II);
    bareiss->smNewBareiss(x, y);
    m = bareiss->smRes2Mod();
    v = NewIntvec1(bareiss->smGetRed());
    bareiss->smToIntvec(v);
    delete bareiss;
    if (origR!=NULL)
    {
      rChangeCurrRing(origR,TRUE);
      mm=idInit(IDELEMS(m),m->rank);
      int k;
      for (k=0;k<IDELEMS(m);k++) mm->m[k] = prCopyR( m->m[k], &tmpR);
      rChangeCurrRing(&tmpR,FALSE);
      idDelete(&m);
      smRingClean(origR,tmpR);
    }
    else
    {
      mm=m;
    }
  }
  res->Init(2);
  res->m[0].rtyp=MODUL_CMD;
  res->m[0].data=(void *)mm;
  res->m[1].rtyp=INTVEC_CMD;
  res->m[1].data=(void *)v;
  return res;
}

/*
* constructor
*/
sparse_mat::sparse_mat(ideal smat)
{
  int i;
  polyset pmat;

  ncols = smat->ncols;
  nrows = idRankFreeModule(smat);
  if (nrows <= 0)
  {
    m_act = NULL;
    return;
  }
  sign = 1;
  inred = act = ncols;
  crd = 0;
  tored = nrows; // without border
  i = tored+1;
  perm = (Exponent_t *)Alloc(sizeof(Exponent_t)*(i+1));
  perm[i] = 0;
  m_row = (smpoly *)Alloc0(sizeof(smpoly)*i);
  wrw = (float *)Alloc(sizeof(float)*i);
  i = ncols+1;
  wcl = (float *)Alloc(sizeof(float)*i);
  m_act = (smpoly *)Alloc(sizeof(smpoly)*i);
  m_res = (smpoly *)Alloc0(sizeof(smpoly)*i);
  dumm = (smpoly)AllocSizeOf(smprec);
  m_res[0] = (smpoly)AllocSizeOf(smprec);
  m_res[0]->m = NULL;
  pmat = smat->m;
  for(i=ncols; i; i--)
  {
    m_act[i] = smPoly2Smpoly(pmat[i-1]);
    pmat[i-1] = NULL;
  }
  this->smZeroElim();
  oldpiv = NULL;
}

/*
* destructor
*/
sparse_mat::~sparse_mat()
{
  int i;
  if (m_act == NULL) return;
  FreeSizeOf((ADDRESS)m_res[0], smprec);
  FreeSizeOf((ADDRESS)dumm, smprec);
  i = ncols+1;
  Free((ADDRESS)m_res, sizeof(smpoly)*i);
  Free((ADDRESS)m_act, sizeof(smpoly)*i);
  Free((ADDRESS)wcl, sizeof(float)*i);
  i = nrows+1;
  Free((ADDRESS)wrw, sizeof(float)*i);
  Free((ADDRESS)m_row, sizeof(smpoly)*i);
  Free((ADDRESS)perm, sizeof(Exponent_t)*(i+1));
}

/*
* transform the result to a module
*/
ideal sparse_mat::smRes2Mod()
{
  ideal res = idInit(crd, crd);
  int i;

  for (i=crd; i; i--) res->m[i-1] = smSmpoly2Poly(m_res[i]);
  res->rank = idRankFreeModule(res);
  return res;
}

/*
* permutation of rows
*/
void sparse_mat::smToIntvec(intvec *v)
{
  int i;

  for (i=v->rows()-1; i>=0; i--)
    (*v)[i] = perm[i+1];
}

/* ---------------- the algorithm's ------------------ */
/*
* the determinant (up to sign), uses new Bareiss elimination
*/
poly sparse_mat::smDet()
{
  int y;
  poly res = NULL;

  if (sign == 0)
  {
    this->smActDel();
    return NULL;
  }
  if (act < 2)
  {
    if (act != 0) res = m_act[1]->m;
    FreeSizeOf((void *)m_act[1], smprec);
    return res;
  }
  normalize = 0;
  this->smInitPerm();
  this->smPivot();
  this->smSign();
  this->smSelectPR();
  this->sm1Elim();
  crd++;
  m_res[crd] = piv;
  this->smColDel();
  act--;
  this->smZeroElim();
  if (sign == 0)
  {
    this->smActDel();
    return NULL;
  }
  if (act < 2)
  {
    this->smFinalMult();
    this->smPivDel();
    if (act != 0) res = m_act[1]->m;
    FreeSizeOf((void *)m_act[1], smprec);
    return res;
  }
  loop
  {
    this->smNewPivot();
    this->smSign();
    this->smSelectPR();
    this->smMultCol();
    this->smHElim();
    crd++;
    m_res[crd] = piv;
    this->smColDel();
    act--;
    this->smZeroElim();
    if (sign == 0)
    {
      this->smPivDel();
      this->smActDel();
      return NULL;
    }
    if (act < 2)
    {
      this->smFinalMult();
      this->smPivDel();
      if (act != 0) res = m_act[1]->m;
      FreeSizeOf((void *)m_act[1], smprec);
      return res;
    }
  }
}

/*
* the Bareiss elimination:
*   - with x unreduced last rows, pivots from here are not allowed
*   - the method will finish for number of unreduced columns < y
*/
void sparse_mat::smBareiss(int x, int y)
{
  if ((x > 0) && (x < nrows))
  {
    tored -= x;
    this->smToredElim();
  }
  if (y < 1) y = 1;
  if (act <= y)
  {
    this->smCopToRes();
    return;
  }
  normalize = this->smCheckNormalize();
  if (normalize) this->smNormalize();
  this->smPivot();
  this->smSelectPR();
  this->smElim();
  crd++;
  this->smColToRow();
  act--;
  this->smRowToCol();
  this->smZeroElim();
  if (tored != nrows)
    this->smToredElim();
  if (act < y)
  {
    this->smCopToRes();
    return;
  }
  loop
  {
    if (normalize) this->smNormalize();
    this->smPivot();
    oldpiv = piv;
    this->smSelectPR();
    this->smElim();
    crd++;
    this->smColToRow();
    act--;
    this->smRowToCol();
    this->smZeroElim();
    if (tored != nrows)
      this->smToredElim();
    if (act < y)
    {
      this->smCopToRes();
      return;
    }
  }
}

/*
* the new Bareiss elimination:
*   - with x unreduced last rows, pivots from here are not allowed
*   - the method will finish for number of unreduced columns < y
*/
void sparse_mat::smNewBareiss(int x, int y0)
{
  int y=y0;
  if ((x > 0) && (x < nrows))
  {
    tored -= x;
    this->smToredElim();
  }
  if (y < 1) y = 1;
  if (act <= y)
  {
    this->smCopToRes();
    return;
  }
  normalize = this->smCheckNormalize();
  if (normalize) this->smNormalize();
  if(y0>=0)
    this->smPivot();
  else
    this->smRealPivot();
  this->smSelectPR();
  this->sm1Elim();
  crd++;
  this->smColToRow();
  act--;
  this->smRowToCol();
  this->smZeroElim();
  if (tored != nrows)
    this->smToredElim();
  if (act <= y)
  {
    this->smFinalMult();
    this->smCopToRes();
    return;
  }
  loop
  {
    if (normalize) this->smNormalize();
    if(y0>=0)
      this->smNewPivot();
    else
      this->smRealPivot();
    this->smSelectPR();
    this->smMultCol();
    this->smHElim();
    crd++;
    this->smColToRow();
    act--;
    this->smRowToCol();
    this->smZeroElim();
    if (tored != nrows)
      this->smToredElim();
    if (act <= y)
    {
      this->smFinalMult();
      this->smCopToRes();
      return;
    }
  }
}

/* ----------------- pivot method ------------------ */

void sparse_mat::smRealPivot()
{
  smpoly a;
  number nopt,n1;
  int i, copt, ropt;

  nopt=nInit(0);
  for (i=act; i; i--)
  {
    a = m_act[i];
    loop
    {
      if (a->pos > tored)
        break;
      n1=pGetCoeff(a->m);
      if(nGreaterZero(n1))
      {
        if(nGreater(n1,nopt))
        {
          nDelete(&nopt);
          nopt=nCopy(n1);
          copt=i;
          ropt=a->pos;
        }
      }
      else
      {
        n1=nNeg(n1);
        if(nGreater(n1,nopt))
        {
          nDelete(&nopt);
          nopt=nCopy(n1);
          copt=i;
          ropt=a->pos;
        }
        n1=nNeg(n1);
      }
      a = a->n;
      if (a == NULL)
        break;
    }
  }
  rpiv = ropt;
  cpiv = copt;
  nDelete(&nopt);
  if (cpiv != act)
  {
    a = m_act[act];
    m_act[act] = m_act[cpiv];
    m_act[cpiv] = a;
  }
}

/*
* prepare smPivot, compute weights for rows and columns
* and the weight for all points
*/
void sparse_mat::smWeights()
{
  float wc, wp, w;
  smpoly a;
  int i;

  wp = 0.0;
  for (i=tored; i; i--) wrw[i] = 0.0; // ???
  for (i=act; i; i--)
  {
    wc = 0.0;
    a = m_act[i];
    loop
    {
      if (a->pos > tored)
        break;
      w = a->f = smPolyWeight(a);
      wc += w;
      wrw[a->pos] += w;
      a = a->n;
      if (a == NULL)
        break;
    }
    wp += wc;
    wcl[i] = wc;
  }
  wpoints = wp;
}

/*
* compute pivot
*/
void sparse_mat::smPivot()
{
  float wopt = 1.0e30;
  float wc, wr, wp, w;
  smpoly a;
  int i, copt, ropt;

  this->smWeights();
  for (i=act; i; i--)
  {
    a = m_act[i];
    loop
    {
      if (a->pos > tored)
        break;
      w = a->f;
      wc = wcl[i]-w;
      wr = wrw[a->pos]-w;
      if ((wr<0.25) || (wc<0.25)) // row or column with only one point
      {
        if (w<wopt)
        {
          wopt = w;
          copt = i;
          ropt = a->pos;
        }
      }
      else // elimination
      {
        wp = w*(wpoints-wcl[i]-wr);
        wp += wr*wc;
        if (wp < wopt)
        {
          wopt = wp;
          copt = i;
          ropt = a->pos;
        }
      }
      a = a->n;
      if (a == NULL)
        break;
    }
  }
  rpiv = ropt;
  cpiv = copt;
  if (cpiv != act)
  {
    a = m_act[act];
    m_act[act] = m_act[cpiv];
    m_act[cpiv] = a;
  }
}

/*
* prepare smPivot, compute weights for rows and columns
* and the weight for all points
*/
void sparse_mat::smNewWeights()
{
  float wc, wp, w, hp = piv->f;
  smpoly a;
  int i, f, e = crd;

  wp = 0.0;
  for (i=tored; i; i--) wrw[i] = 0.0; // ???
  for (i=act; i; i--)
  {
    wc = 0.0;
    a = m_act[i];
    loop
    {
      if (a->pos > tored)
        break;
      w = a->f;
      f = a->e;
      if (f < e)
      {
        w *= hp;
        if (f) w /= m_res[f]->f;
      }
      wc += w;
      wrw[a->pos] += w;
      a = a->n;
      if (a == NULL)
        break;
    }
    wp += wc;
    wcl[i] = wc;
  }
  wpoints = wp;
}

/*
* compute pivot
*/
void sparse_mat::smNewPivot()
{
  float wopt = 1.0e30, hp = piv->f;
  float wc, wr, wp, w;
  smpoly a;
  int i, copt, ropt, f, e = crd;

  this->smNewWeights();
  for (i=act; i; i--)
  {
    a = m_act[i];
    loop
    {
      if (a->pos > tored)
        break;
      w = a->f;
      f = a->e;
      if (f < e)
      {
        w *= hp;
        if (f) w /= m_res[f]->f;
      }
      wc = wcl[i]-w;
      wr = wrw[a->pos]-w;
      if ((wr<0.25) || (wc<0.25)) // row or column with only one point
      {
        if (w<wopt)
        {
          wopt = w;
          copt = i;
          ropt = a->pos;
        }
      }
      else // elimination
      {
        wp = w*(wpoints-wcl[i]-wr);
        wp += wr*wc;
        if (wp < wopt)
        {
          wopt = wp;
          copt = i;
          ropt = a->pos;
        }
      }
      a = a->n;
      if (a == NULL)
        break;
    }
  }
  rpiv = ropt;
  cpiv = copt;
  if (cpiv != act)
  {
    a = m_act[act];
    m_act[act] = m_act[cpiv];
    m_act[cpiv] = a;
  }
}

/* ----------------- elimination ------------------ */

/* steps of elimination */
void sparse_mat::smElim()
{
  poly p = piv->m;        // pivotelement
  smpoly c = m_act[act];  // pivotcolumn
  smpoly r = red;         // row to reduce
  poly q;
  smpoly res, a, b;
  poly w, ha, hb;
  int i;

  if (oldpiv != NULL) q = oldpiv->m; // previous pivot
  else q = NULL;
  if ((c == NULL) || (r == NULL))
  {
    while (r) smElemDelete(&r);
    for (i=1; i<act; i++)
    {
      a = m_act[i];
      while (a != NULL)
      {
        ha = SM_MULT(a->m, p, q);
        pDelete(&a->m);
        if (q) SM_DIV(ha, q);
        a->m = ha;
        a = a->n;
      }
    }
    return;
  }
  for (i=1; i<act; i++)
  {
    a = m_act[i];
    if ((r == NULL) || (i != r->pos))  // cols without elimination
    {
      while (a != NULL)
      {
        ha = SM_MULT(a->m, p, q);
        pDelete(&a->m);
        if (q) SM_DIV(ha, q);
        a->m = ha;
        a = a->n;
      }
    }
    else                    // cols with elimination
    {
      res = dumm;
      res->n = NULL;
      b = c;
      w = r->m;
      loop                  // combine the chains a and b: p*a + w*b
      {
        if (a == NULL)
        {
          if (b != NULL)
          {
            do
            {
              res = res->n = smElemCopy(b);
              hb = SM_MULT(b->m, w, q);
              if (q) SM_DIV(hb, q);
              res->m = hb;
              b = b->n;
            } while (b != NULL);
          }
          else
            res->n = NULL;
          break;
        }
        if (b == NULL)
        {
          do
          {
            ha = SM_MULT(a->m, p, q);
            pDelete(&a->m);
            if (q) SM_DIV(ha, q);
            a->m = ha;
            res = res->n = a;
            a = a->n;
          } while (a != NULL);
          break;
        }
        if (a->pos < b->pos)
        {
          ha = SM_MULT(a->m, p, q);
          pDelete(&a->m);
          if (q) SM_DIV(ha, q);
          a->m = ha;
          res = res->n = a;
          a = a->n;
        }
        else if (a->pos > b->pos)
        {
          res = res->n = smElemCopy(b);
          hb = SM_MULT(b->m, w, q);
          b = b->n;
          if (q) SM_DIV(hb, q);
          res->m = hb;
        }
        else
        {
          ha = SM_MULT(a->m, p, q);
          pDelete(&a->m);
          hb = SM_MULT(b->m, w, q);
          ha = pAdd(ha, hb);
          if (ha != NULL)
          {
            if (q) SM_DIV(ha, q);
            a->m = ha;
            res = res->n = a;
            a = a->n;
          }
          else
          {
            smElemDelete(&a);
          }
          b = b->n;
        }
      }
      m_act[i] = dumm->n;
      if (r) smElemDelete(&r);
    }
  }
}

/* first step of elimination */
void sparse_mat::sm1Elim()
{
  poly p = piv->m;        // pivotelement
  smpoly c = m_act[act];  // pivotcolumn
  smpoly r = red;         // row to reduce
  smpoly res, a, b;
  poly w, ha, hb;

  if ((c == NULL) || (r == NULL))
  {
    while (r!=NULL) smElemDelete(&r);
    return;
  }
  do
  {
    a = m_act[r->pos];
    res = dumm;
    res->n = NULL;
    b = c;
    w = r->m;
    loop                  // combine the chains a and b: p*a + w*b
    {
      if (a == NULL)
      {
        do
        {
          res = res->n = smElemCopy(b);
          res->m = smMult(b->m, w);
          res->e = 1;
          res->f = smPolyWeight(res);
          b = b->n;
        } while (b != NULL);
        break;
      }
      if (a->pos < b->pos)
      {
        res = res->n = a;
        a = a->n;
      }
      else if (a->pos > b->pos)
      {
        res = res->n = smElemCopy(b);
        res->m = smMult(b->m, w);
        res->e = 1;
        res->f = smPolyWeight(res);
        b = b->n;
      }
      else
      {
        ha = smMult(a->m, p);
        pDelete(&a->m);
        hb = smMult(b->m, w);
        ha = pAdd(ha, hb);
        if (ha != NULL)
        {
          a->m = ha;
          a->e = 1;
          a->f = smPolyWeight(a);
          res = res->n = a;
          a = a->n;
        }
        else
        {
          smElemDelete(&a);
        }
        b = b->n;
      }
      if (b == NULL)
      {
        res->n = a;
        break;
      }
    }
    m_act[r->pos] = dumm->n;
    smElemDelete(&r);
  } while (r != NULL);
}

/* higher steps of elimination */
void sparse_mat::smHElim()
{
  poly hp = this->smMultPoly(piv);
  poly gp = piv->m;       // pivotelement
  smpoly c = m_act[act];  // pivotcolumn
  smpoly r = red;         // row to reduce
  smpoly res, a, b;
  poly ha, hr, x, y;
  int e, ip, ir, ia, lev;

  if ((c == NULL) || (r == NULL))
  {
    while(r!=NULL) smElemDelete(&r);
    pDelete(&hp);
    return;
  }
  e = crd+1;
  ip = piv->e;
  do
  {
    a = m_act[r->pos];
    res = dumm;
    res->n = NULL;
    b = c;
    hr = r->m;
    ir = r->e;
    loop                  // combine the chains a and b: (hp,gp)*a(l) + hr*b(h)
    {
      if (a == NULL)
      {
        do
        {
          res = res->n = smElemCopy(b);
          x = SM_MULT(b->m, hr, m_res[ir]->m);
          b = b->n;
          if(ir) SM_DIV(x, m_res[ir]->m);
          res->m = x;
          res->e = e;
          res->f = smPolyWeight(res);
        } while (b != NULL);
        break;
      }
      if (a->pos < b->pos)
      {
        res = res->n = a;
        a = a->n;
      }
      else if (a->pos > b->pos)
      {
        res = res->n = smElemCopy(b);
        x = SM_MULT(b->m, hr, m_res[ir]->m);
        b = b->n;
        if(ir) SM_DIV(x, m_res[ir]->m);
        res->m = x;
        res->e = e;
        res->f = smPolyWeight(res);
      }
      else
      {
        ha = a->m;
        ia = a->e;
        if (ir >= ia)
        {
          if (ir > ia)
          {
            x = SM_MULT(ha, m_res[ir]->m, m_res[ia]->m);
            pDelete(&ha);
            ha = x;
            if (ia) SM_DIV(ha, m_res[ia]->m);
            ia = ir;
          }
          x = SM_MULT(ha, gp, m_res[ia]->m);
          pDelete(&ha);
          y = SM_MULT(b->m, hr, m_res[ia]->m);
        }
        else if (ir >= ip)
        {
          if (ia < crd)
          {
            x = SM_MULT(ha, m_res[crd]->m, m_res[ia]->m);
            pDelete(&ha);
            ha = x;
            SM_DIV(ha, m_res[ia]->m);
          }
          y = hp;
          if(ir > ip)
          {
            y = SM_MULT(y, m_res[ir]->m, m_res[ip]->m);
            if (ip) SM_DIV(y, m_res[ip]->m);
          }
          ia = ir;
          x = SM_MULT(ha, y, m_res[ia]->m);
          if (y != hp) pDelete(&y);
          pDelete(&ha);
          y = SM_MULT(b->m, hr, m_res[ia]->m);
        }
        else
        {
          x = SM_MULT(hr, m_res[ia]->m, m_res[ir]->m);
          if (ir) SM_DIV(x, m_res[ir]->m);
          y = SM_MULT(b->m, x, m_res[ia]->m);
          pDelete(&x);
          x = SM_MULT(ha, gp, m_res[ia]->m);
          pDelete(&ha);
        }
        ha = pAdd(x, y);
        if (ha != NULL)
        {
          if (ia) SM_DIV(ha, m_res[ia]->m);
          a->m = ha;
          a->e = e;
          a->f = smPolyWeight(a);
          res = res->n = a;
          a = a->n;
        }
        else
        {
          a->m = NULL;
          smElemDelete(&a);
        }
        b = b->n;
      }
      if (b == NULL)
      {
        res->n = a;
        break;
      }
    }
    m_act[r->pos] = dumm->n;
    smElemDelete(&r);
  } while (r != NULL);
  pDelete(&hp);
}

/* ----------------- transfer ------------------ */

/*
* select the pivotrow and store it to red and piv
*/
void sparse_mat::smSelectPR()
{
  smpoly b = dumm;
  smpoly a, ap;
  int i;

  a = m_act[act];
  if (a->pos < rpiv)
  {
    do
    {
      ap = a;
      a = a->n;
    } while (a->pos < rpiv);
    ap->n = a->n;
  }
  else
    m_act[act] = a->n;
  piv = a;
  a->n = NULL;
  for (i=1; i<act; i++)
  {
    a = m_act[i];
    if (a->pos < rpiv)
    {
      loop
      {
        ap = a;
        a = a->n;
        if ((a == NULL) || (a->pos > rpiv))
          break;
        if (a->pos == rpiv)
        {
          ap->n = a->n;
          a->m = pNeg(a->m);
          b = b->n = a;
          b->pos = i;
          break;
        }
      }
    }
    else if (a->pos == rpiv)
    {
      m_act[i] = a->n;
      a->m = pNeg(a->m);
      b = b->n = a;
      b->pos = i;
    }
  }
  b->n = NULL;
  red = dumm->n;
}

/*
* store the pivotcol in m_row
*   m_act[cols][pos(rows)] => m_row[rows][pos(cols)]
*/
void sparse_mat::smColToRow()
{
  smpoly c = m_act[act];
  smpoly h;

  while (c != NULL)
  {
    h = c;
    c = c->n;
    h->n = m_row[h->pos];
    m_row[h->pos] = h;
    h->pos = crd;
  }
}

/*
* store the pivot and the assosiated row in m_row
* to m_res (result):
*   piv + m_row[rows][pos(cols)] => m_res[cols][pos(rows)]
*/
void sparse_mat::smRowToCol()
{
  smpoly r = m_row[rpiv];
  smpoly a, ap, h;

  m_row[rpiv] = NULL;
  perm[crd] = rpiv;
  piv->pos = crd;
  m_res[crd] = piv;
  while (r != NULL)
  {
    ap = m_res[r->pos];
    loop
    {
      a = ap->n;
      if (a == NULL)
      {
        ap->n = h = r;
        r = r->n;
        h->n = a;
        h->pos = crd;
        break;
      }
      ap = a;
    }
  }
}

/* ----------------- C++ stuff ------------------ */

/*
*  clean m_act from zeros (after elim)
*/
void sparse_mat::smZeroElim()
{
  int i = 0;
  int j;

  loop
  {
    i++;
    if (i > act) return;
    if (m_act[i] == NULL) break;
  }
  j = i;
  loop
  {
    j++;
    if (j > act) break;
    if (m_act[j] != NULL)
    {
      m_act[i] = m_act[j];
      i++;
    }
  }
  act -= (j-i);
  sign = 0;
}

/*
*  clean m_act from cols not to reduced (after elim)
*  put them to m_res
*/
void sparse_mat::smToredElim()
{
  int i = 0;
  int j;

  loop
  {
    i++;
    if (i > act) return;
    if (m_act[i]->pos > tored)
    {
      m_res[inred] = m_act[i];
      inred--;
      break;
    }
  }
  j = i;
  loop
  {
    j++;
    if (j > act) break;
    if (m_act[j]->pos > tored)
    {
      m_res[inred] = m_act[j];
      inred--;
    }
    else
    {
      m_act[i] = m_act[j];
      i++;
    }
  }
  act -= (j-i);
  sign = 0;
}

/*
*  copy m_act to m_res
*/
void sparse_mat::smCopToRes()
{
  smpoly a,ap,r,h;
  int i,j,k,l;

  i = 0;
  if (act)
  {
    a = m_act[act]; // init perm
    do
    {
      i++;
      perm[crd+i] = a->pos;
      a = a->n;
    } while ((a != NULL) && (a->pos <= tored));
    for (j=act-1;j;j--) // load all positions of perm
    {
      a = m_act[j];
      k = 1;
      loop
      {
        if (perm[crd+k] >= a->pos)
        {
          if (perm[crd+k] > a->pos)
          {
            for (l=i;l>=k;l--) perm[crd+l+1] = perm[crd+l];
            perm[crd+k] = a->pos;
            i++;
          }
          a = a->n;
          if ((a == NULL) || (a->pos > tored)) break;
        }
        k++;
        if ((k > i) && (a->pos <= tored))
        {
          do
          {
            i++;
            perm[crd+i] = a->pos;
            a = a->n;
          } while ((a != NULL) && (a->pos <= tored));
          break;
        }
      }
    }
  }
  for (j=act;j;j--) // renumber m_act
  {
    k = 1;
    a = m_act[j];
    while ((a != NULL) && (a->pos <= tored))
    {
      if (perm[crd+k] == a->pos)
      {
        a->pos = crd+k;
        a = a->n;
      }
      k++;
    }
  }
  tored = crd+i;
  for(k=1;k<=i;k++) // clean this from m_row
  {
    j = perm[crd+k];
    if (m_row[j] != NULL)
    {
      r = m_row[j];
      m_row[j] = NULL;
      do
      {
        ap = m_res[r->pos];
        loop
        {
          a = ap->n;
          if (a == NULL)
          {
            h = ap->n = r;
            r = r->n;
            h->n = NULL;
            h->pos = crd+k;
            break;
          }
          ap = a;
        }
      } while (r!=NULL);
    }
  }
  while(act) // clean m_act
  {
    crd++;
    m_res[crd] = m_act[act];
    act--;
  }
  for (i=1;i<=tored;i++) // take the rest of m_row
  {
    if(m_row[i] != NULL)
    {
      tored++;
      r = m_row[i];
      m_row[i] = NULL;
      perm[tored] = i;
      do
      {
        ap = m_res[r->pos];
        loop
        {
          a = ap->n;
          if (a == NULL)
          {
            h = ap->n = r;
            r = r->n;
            h->n = NULL;
            h->pos = tored;
            break;
          }
          ap = a;
        }
      } while (r!=NULL);
    }
  }
  for (i=tored+1;i<=nrows;i++) // take the rest of m_row
  {
    if(m_row[i] != NULL)
    {
      r = m_row[i];
      m_row[i] = NULL;
      do
      {
        ap = m_res[r->pos];
        loop
        {
          a = ap->n;
          if (a == NULL)
          {
            h = ap->n = r;
            r = r->n;
            h->n = NULL;
            h->pos = i;
            break;
          }
          ap = a;
        }
      } while (r!=NULL);
    }
  }
  while (inred < ncols) // take unreducable
  {
    crd++;
    inred++;
    m_res[crd] = m_res[inred];
  }
}

/*
* multiply and divide the column, that goes to result
*/
void sparse_mat::smMultCol()
{
  smpoly a = m_act[act];
  int e = crd;
  poly ha;
  int f;

  while (a != NULL)
  {
    f = a->e;
    if (f < e)
    {
      ha = SM_MULT(a->m, m_res[e]->m, m_res[f]->m);
      pDelete(&a->m);
      if (f) SM_DIV(ha, m_res[f]->m);
      a->m = ha;
      if (normalize) pNormalize(a->m);
    }
    a = a->n;
  }
}

/*
* multiply and divide the m_act finaly
*/
void sparse_mat::smFinalMult()
{
  smpoly a;
  poly ha;
  int i, f;
  int e = crd;

  for (i=act; i; i--)
  {
    a = m_act[i];
    do
    {
      f = a->e;
      if (f < e)
      {
        ha = SM_MULT(a->m, m_res[e]->m, m_res[f]->m);
        pDelete(&a->m);
        if (f) SM_DIV(ha, m_res[f]->m);
        a->m = ha;
      }
      if (normalize) pNormalize(a->m);
      a = a->n;
    } while (a != NULL);
  }
}

/*
* check for denominators
*/
int sparse_mat::smCheckNormalize()
{
  int i;
  smpoly a;

  for (i=act; i; i--)
  {
    a = m_act[i];
    do
    {
      if(smHaveDenom(a->m)) return 1;
      a = a->n;
    } while (a != NULL);
  }
  return 0;
}

/*
* normalize
*/
void sparse_mat::smNormalize()
{
  smpoly a;
  int i;
  int e = crd;

  for (i=act; i; i--)
  {
    a = m_act[i];
    do
    {
      if (e == a->e) pNormalize(a->m);
      a = a->n;
    } while (a != NULL);
  }
}

/*
* multiply and divide the element, save poly
*/
poly sparse_mat::smMultPoly(smpoly a)
{
  int f = a->e;
  poly r, h;

  if (f < crd)
  {
    h = r = a->m;
    h = SM_MULT(h, m_res[crd]->m, m_res[f]->m);
    if (f) SM_DIV(h, m_res[f]->m);
    a->m = h;
    if (normalize) pNormalize(a->m);
    a->f = smPolyWeight(a);
    return r;
  }
  else
    return NULL;
}

/*
* delete the m_act finaly
*/
void sparse_mat::smActDel()
{
  smpoly a;
  int i;

  for (i=act; i; i--)
  {
    a = m_act[i];
    do
    {
      smElemDelete(&a);
    } while (a != NULL);
  }
}

/*
* delete the pivotcol
*/
void sparse_mat::smColDel()
{
  smpoly a = m_act[act];

  while (a != NULL)
  {
    smElemDelete(&a);
  }
}

/*
* delete pivot elements
*/
void sparse_mat::smPivDel()
{
  int i=crd;

  while (i != 0)
  {
    smElemDelete(&m_res[i]);
    i--;
  }
}

/*
* the sign of the determinant
*/
void sparse_mat::smSign()
{
  int j,i;
  if (act > 2)
  {
    if (cpiv!=act) sign=-sign;
    if ((act%2)==0) sign=-sign;
    i=1;
    j=perm[1];
    while(j<rpiv)
    {
      sign=-sign;
      i++;
      j=perm[i];
    }
    while(perm[i]!=0)
    {
      perm[i]=perm[i+1];
      i++;
    }
  }
  else
  {
    if (cpiv!=1) sign=-sign;
    if (rpiv!=perm[1]) sign=-sign;
  }
}

void sparse_mat::smInitPerm()
{
  int i;
  for (i=act;i;i--) perm[i]=i;
}

/* ----------------- arithmetic ------------------ */

/*
*  returns a*b
*  a,b NOT destroyed
*/
poly smMult(poly a, poly b)
{
  poly pa, res, r;

  if (smSmaller(a, b))
  {
    r = a;
    a = b;
    b = r;
  }
  if (pNext(b) == NULL)
  {
    if (pIsConstantComp(b))
      return pMultCopyN(a, pGetCoeff(b));
    else
      return smEMult(a, b);
  }
  pa = res = smEMult(a, b);
  pIter(b);
  do
  {
    r = smEMult(a, b);
    smCombineChain(&pa, r);
    pIter(b);
  } while (b != NULL);
  return res;
}

/*2
* exact division a/b
* a destroyed, b NOT destroyed
*/
void smPolyDiv(poly a, poly b)
{
  const number x = pGetCoeff(b);
  number y, yn;
  poly t, h, dummy;
  int i;

  if (pNext(b) == NULL)
  {
    do
    {
      if (!pIsConstantComp(b))
      {
        for (i=pVariables; i; i--)
          pSubExp(a,i,pGetExp(b,i));
        pSetm(a);
      }
      y = nDiv(pGetCoeff(a),x);
      nNormalize(y);
      pSetCoeff(a,y);
      pIter(a);
    } while (a != NULL);
    return;
  }
  dummy = pInit();
  do
  {
    for (i=pVariables; i; i--)
      pSubExp(a,i,pGetExp(b,i));
    pSetm(a);
    y = nDiv(pGetCoeff(a),x);
    nNormalize(y);
    pSetCoeff(a,y);
    yn = nNeg(nCopy(y));
    t = pNext(b);
    h = dummy;
    do
    {
      h = pNext(h) = pInit();
      //pSetComp(h,0);
      for (i=pVariables; i; i--)
        pSetExp(h,i,pGetExp(a,i)+pGetExp(t,i));
      pSetm(h);
      pSetCoeff0(h,nMult(yn, pGetCoeff(t)));
      pIter(t);
    } while (t != NULL);
    nDelete(&yn);
    pNext(h) = NULL;
    a = pNext(a) = pAdd(pNext(a), pNext(dummy));
  } while (a!=NULL);
  pFree1(dummy);
}

/*
*  returns the part of (a*b)/exp(lead(c)) with nonegative exponents
*/
poly smMultDiv(poly a, poly b, const poly c)
{
  poly pa, e, res, r;
  BOOLEAN lead;

  if (smSmaller(a, b))
  {
    r = a;
    a = b;
    b = r;
  }
  if ((c == NULL) || pIsConstantComp(c))
  {
    if (pNext(b) == NULL)
    {
      if (pIsConstantComp(b))
        return pMultCopyN(a, pGetCoeff(b));
      else
        return smEMult(a, b);
    }
    pa = res = smEMult(a, b);
    pIter(b);
    do
    {
      r = smEMult(a, b);
      smCombineChain(&pa, r);
      pIter(b);
    } while (b != NULL);
    return res;
  }
  res = NULL;
  e = pInit();
  lead = FALSE;
  while (!lead)
  {
    pSetCoeff0(e,pGetCoeff(b));
    if (smIsNegQuot(e, b, c))
    {
      lead = smCheckLead(a, e);
      r = smDMult(a, e);
    }
    else
    {
      lead = TRUE;
      r = smEMult(a, e);
    }
    if (lead)
    {
      if (res != NULL)
      {
        smFindRef(&pa, &res, r);
        if (pa == NULL)
          lead = FALSE;
      }
      else
      {
        pa = res = r;
      }
    }
    else
      res = pAdd(res, r);
    pIter(b);
    if (b == NULL)
    {
      pFree1(e);
      return res;
    }
  }
  do
  {
    pSetCoeff0(e,pGetCoeff(b));
    if (smIsNegQuot(e, b, c))
    {
      r = smDMult(a, e);
      if (smCheckLead(a, e))
        smCombineChain(&pa, r);
      else
        pa = pAdd(pa,r);
    }
    else
    {
      r = smEMult(a, e);
      smCombineChain(&pa, r);
    }
    pIter(b);
  } while (b != NULL);
  pFree1(e);
  return res;
}

/*n
* exact division a/b
* a is a result of smMultDiv
* a destroyed, b NOT destroyed
*/
void smSpecialPolyDiv(poly a, poly b)
{
  if (pNext(b) == NULL)
  {
    smPolyDivN(a, pGetCoeff(b));
    return;
  }
  smExactPolyDiv(a, b);
}

/* ------------ internals arithmetic ------------- */
static void smExactPolyDiv(poly a, poly b)
{
  const number x = pGetCoeff(b);
  poly tail = pNext(b), e = pInit();
  poly h;
  number y, yn;

  do
  {
    y = nDiv(pGetCoeff(a), x);
    nNormalize(y);
    pSetCoeff(a,y);
    yn = nNeg(nCopy(y));
    pSetCoeff0(e,yn);
    if (smIsNegQuot(e, a, b))
      h = smDMult(tail, e);
    else
      h = smEMult(tail, e);
    nDelete(&yn);
    a = pNext(a) = pAdd(pNext(a), h);
  } while (a!=NULL);
  pFree1(e);
}

static BOOLEAN smIsNegQuot(poly a, const poly b, const poly c)
{
  int i;

  for (i=pVariables; i; i--)
  {
    pSetExp(a,i,pGetExp(b,i)-pGetExp(c,i));
    if (pGetExp(a,i) < 0)
    {
      while(--i) pSetExp(a,i,pGetExp(b,i)-pGetExp(c,i));
      return TRUE;
    }
  }
  return FALSE;
}

static poly smEMult(poly t, const poly e)
{
  const number y = pGetCoeff(e);
  poly res, h;
  int i;

  h = res = pInit();
  loop
  {
    //pSetComp(h,0);
    for (i=pVariables; i; i--)
      pSetExp(h,i,pGetExp(e,i)+pGetExp(t,i));
    pSetm(h);
    pSetCoeff0(h,nMult(y,pGetCoeff(t)));
    pIter(t);
    if (t == NULL)
    {
      pNext(h) = NULL;
      return res;
    }
    h = pNext(h) = pInit();
  }
}

static BOOLEAN smCheckLead(const poly t, const poly e)
{
  int i;
  for (i=pVariables; i; i--)
  {
    if ((pGetExp(e,i)+pGetExp(t,i)) < 0)
      return FALSE;
  }
  return TRUE;
}

static poly smDMult(poly t, const poly e)
{
  const number y = pGetCoeff(e);
  poly r = NULL;
  poly res, h;
  int i;
  Exponent_t w;

  h = res = pInit();
  loop
  {
    i=pVariables;
    loop
    {
      w = pGetExp(e,i)+pGetExp(t,i);
      if (w < 0) break;
      pSetExp(h,i,w);
      i--;
      if (i == 0)
      {
        pSetm(h);
        pSetCoeff0(h,nMult(y,pGetCoeff(t)));
        pIter(t);
        if (t == NULL)
        {
          pNext(h) = NULL;
          return res;
        }
        r = h;
        h = pNext(h) = pInit();
        i=pVariables;
      }
    }
    pIter(t);
    if (t == NULL)
    {
      if (r != NULL)
        pNext(r) = NULL;
      else
        res = NULL;
      pFree1(h);
      return res;
    }
  }
}

static void smPolyDivN(poly a, const number x)
{
  number y;

  do
  {
    y = nDiv(pGetCoeff(a),x);
    nNormalize(y);
    pSetCoeff(a,y);
    pIter(a);
  } while (a != NULL);
}

static BOOLEAN smSmaller(poly a, poly b)
{
  loop
  {
    pIter(b);
    if (b == NULL) return TRUE;
    pIter(a);
    if (a == NULL) return FALSE;
  }
}

static void smCombineChain(poly *px, poly r)
{
  poly pa = *px, pb;
  number x;
  int i;

  loop
  {
    pb = pNext(pa);
    if (pb == NULL)
    {
      pa = pNext(pa) = r;
      break;
    }
    i = pComp0(pb, r);
    if (i > 0)
      pa = pb;
    else
    {
      if (i == 0)
      {
        x = nAdd(pGetCoeff(pb), pGetCoeff(r));
        pDelete1(&r);
        if (nIsZero(x))
        {
          pDelete1(&pb);
          pNext(pa) = pAdd(pb,r);
        }
        else
        {
          pa = pb;
          pSetCoeff(pa,x);
          pNext(pa) = pAdd(pNext(pa), r);
        }
      }
      else
      {
        pa = pNext(pa) = r;
        pNext(pa) = pAdd(pb, pNext(pa));
      }
      break;
    }
  }
  *px = pa;
}

static void smFindRef(poly *ref, poly *px, poly r)
{
  number x;
  int i;
  poly pa = *px, pp = NULL;

  loop
  {
    i = pComp0(pa, r);
    if (i > 0)
    {
      pp = pa;
      pIter(pa);
      if (pa==NULL)
      {
        pNext(pp) = r;
        break;
      }
    }
    else
    {
      if (i == 0)
      {
        x = nAdd(pGetCoeff(pa), pGetCoeff(r));
        pDelete1(&r);
        if (nIsZero(x))
        {
          pDelete1(&pa);
          if (pp!=NULL)
            pNext(pp) = pAdd(pa,r);
          else
            *px = pAdd(pa,r);
        }
        else
        {
          pp = pa;
          pSetCoeff(pp,x);
          pNext(pp) = pAdd(pNext(pp), r);
        }
      }
      else
      {
        if (pp!=NULL)
          pp = pNext(pp) = r;
        else
          *px = pp = r;
        pNext(pp) = pAdd(pa, pNext(r));
      }
      break;
    }
  }
  *ref = pp;
}

/* ----------------- internal 'C' stuff ------------------ */

static void smElemDelete(smpoly *r)
{
  smpoly a = *r, b = a->n;

  pDelete(&a->m);
  FreeSizeOf((void *)a, smprec);
  *r = b;
}

static smpoly smElemCopy(smpoly a)
{
  smpoly r = (smpoly)AllocSizeOf(smprec);
  memcpy(r, a, sizeof(smprec));
/*  r->m = pCopy(r->m); */
  return r;
}

/*
* from poly to smpoly
* do not destroy p
*/
static smpoly smPoly2Smpoly(poly q)
{
  poly pp;
  smpoly res, a;
  Exponent_t x;

  if (q == NULL)
    return NULL;
  a = res = (smpoly)AllocSizeOf(smprec);
  a->pos = x = pGetComp(q);
  a->m = q;
  a->e = 0;
  loop
  {
    pSetComp(q,0);
    pp = q;
    pIter(q);
    if (q == NULL)
    {
      a->n = NULL;
      return res;
    }
    if (pGetComp(q) != x)
    {
      a = a->n = (smpoly)AllocSizeOf(smprec);
      pNext(pp) = NULL;
      a->pos = x = pGetComp(q);
      a->m = q;
      a->e = 0;
    }
  }
}

/*
* from smpoly to poly
* destroy a
*/
static poly smSmpoly2Poly(smpoly a)
{
  smpoly b;
  poly res, pp, q;
  Exponent_t x;

  if (a == NULL)
    return NULL;
  x = a->pos;
  q = res = a->m;
  loop
  {
    pSetComp(q,x);
    pp = q;
    pIter(q);
    if (q == NULL)
      break;
  }
  loop
  {
    b = a;
    a = a->n;
    FreeSizeOf((void *)b, smprec);
    if (a == NULL)
      return res;
    x = a->pos;
    q = pNext(pp) = a->m;
    loop
    {
      pSetComp(q,x);
      pp = q;
      pIter(q);
      if (q == NULL)
        break;
    }
  }
}

/*
* weigth of a polynomial, for pivot strategy
*/
static float smPolyWeight(smpoly a)
{
  poly p = a->m;
  int i;
  float res = (float)nSize(pGetCoeff(p));

  if (pNext(p) == NULL)
  {
    for(i=pVariables; i>0; i--)
    {
      if (pGetExp(p,i) != 0) return res+1.0;
    }
    return res;
  }
  else
  {
    i = 0;
    res = 0.0;
    do
    {
      i++;
      res += (float)nSize(pGetCoeff(p));
      pIter(p);
    }
    while (p);
    return res+(float)i;
  }
}

static BOOLEAN smHaveDenom(poly a)
{
  BOOLEAN sw;
  number x,o=nInit(1);

  while (a != NULL)
  {
    x = nGetDenom(pGetCoeff(a));
    sw = nEqual(x,o);
    nDelete(&x);
    if (!sw)
    {
      nDelete(&o);
      return TRUE;
    }
    pIter(a);
  }
  nDelete(&o);
  return FALSE;
}

static number smCleardenom(ideal id)
{
  poly a;
  number x,y,res=nInit(1);
  BOOLEAN sw=FALSE;

  for (int i=0; i<IDELEMS(id); i++)
  {
    a = id->m[i];
    sw = smHaveDenom(a);
    if (sw) break;
  }
  if (!sw) return res;
  for (int i=0; i<IDELEMS(id); i++)
  {
    a = id->m[i];
    x = nCopy(pGetCoeff(a));
    pCleardenom(a);
    y = nDiv(x,pGetCoeff(a));
    nDelete(&x);
    x = nMult(res,y);
    nNormalize(x);
    nDelete(&res);
    res = x;
  }
  return res;
}

