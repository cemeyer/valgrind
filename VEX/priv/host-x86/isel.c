
/*---------------------------------------------------------------*/
/*---                                                         ---*/
/*--- This file (host-x86/isel.c) is                          ---*/
/*--- Copyright (c) 2004 OpenWorks LLP.  All rights reserved. ---*/
/*---                                                         ---*/
/*---------------------------------------------------------------*/

/*
   This file is part of LibVEX, a library for dynamic binary
   instrumentation and translation.

   Copyright (C) 2004 OpenWorks, LLP.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; Version 2 dated June 1991 of the
   license.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE, or liability
   for damages.  See the GNU General Public License for more details.

   Neither the names of the U.S. Department of Energy nor the
   University of California nor the names of its contributors may be
   used to endorse or promote products derived from this software
   without prior written permission.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
   USA.
*/

#include "libvex_basictypes.h"
#include "libvex_ir.h"
#include "libvex.h"

#include "main/vex_util.h"
#include "main/vex_globals.h"
#include "host-generic/h_generic_regs.h"
#include "host-x86/hdefs.h"


/*---------------------------------------------------------*/
/*--- Stuff for pattern matching on IR.  This isn't     ---*/
/*--- x86 specific, and should be moved elsewhere.      ---*/
/*---------------------------------------------------------*/

#define DECLARE_PATTERN(_patt) \
   static IRExpr* _patt = NULL

#define DEFINE_PATTERN(_patt,_expr)                            \
   do {                                                        \
      if (!(_patt)) {                                          \
         vassert(LibVEX_GetAllocMode() == AllocModeTEMPORARY); \
         LibVEX_SetAllocMode(AllocModePERMANENT);              \
         _patt = (_expr);                                      \
         LibVEX_SetAllocMode(AllocModeTEMPORARY);              \
         vassert(LibVEX_GetAllocMode() == AllocModeTEMPORARY); \
      }                                                        \
   } while (0)


#define N_MATCH_BINDERS 4
typedef
   struct {
      IRExpr* bindee[N_MATCH_BINDERS];
   }
   MatchInfo;


static void setBindee ( MatchInfo* mi, Int n, IRExpr* bindee )
{
   if (n < 0 || n >= N_MATCH_BINDERS)
      vpanic("setBindee: out of range index");
   if (mi->bindee[n] != NULL)
      vpanic("setBindee: bindee already set");
   mi->bindee[n] = bindee;
}

static Bool matchWrk ( MatchInfo* mi, IRExpr* p/*attern*/, IRExpr* e/*xpr*/ )
{
   switch (p->tag) {
      case Iex_Binder: /* aha, what we were looking for. */
         setBindee(mi, p->Iex.Binder.binder, e);
         return True;
#if 0
      case Iex_GetI:
         if (e->tag != Iex_GetI) return False;
         if (p->Iex.GetI.ty != e->Iex.GetI.ty) return False;
         /* we ignore the offset limit hints .. */
         if (!matchWrk(mi, p->Iex.GetI.offset, e->Iex.GetI.offset))
            return False;
         return True;
#endif
      case Iex_Unop:
         if (e->tag != Iex_Unop) return False;
         if (p->Iex.Unop.op != e->Iex.Unop.op) return False;
         if (!matchWrk(mi, p->Iex.Unop.arg, e->Iex.Unop.arg))
            return False;
         return True;
      case Iex_Binop:
         if (e->tag != Iex_Binop) return False;
         if (p->Iex.Binop.op != e->Iex.Binop.op) return False;
	 if (!matchWrk(mi, p->Iex.Binop.arg1, e->Iex.Binop.arg1))
            return False;
	 if (!matchWrk(mi, p->Iex.Binop.arg2, e->Iex.Binop.arg2))
            return False;
         return True;
      case Iex_LDle:
         if (e->tag != Iex_LDle) return False;
         if (p->Iex.LDle.ty != e->Iex.LDle.ty) return False;
         if (!matchWrk(mi, p->Iex.LDle.addr, e->Iex.LDle.addr))
            return False;
         return True;
      case Iex_Const:
         if (e->tag != Iex_Const) return False;
         return eqIRConst(p->Iex.Const.con, e->Iex.Const.con);
      default: 
         ppIRExpr(p);
         vpanic("match");
   }
}

static Bool matchIRExpr ( MatchInfo* mi, IRExpr* p/*attern*/, IRExpr* e/*xpr*/ )
{
   Int i;
   for (i = 0; i < N_MATCH_BINDERS; i++)
      mi->bindee[i] = NULL;
   return matchWrk(mi, p, e);
}

/*-----*/
/* These are duplicated in x86toIR.c */
static IRExpr* unop ( IROp op, IRExpr* a )
{
   return IRExpr_Unop(op, a);
}

static IRExpr* binop ( IROp op, IRExpr* a1, IRExpr* a2 )
{
   return IRExpr_Binop(op, a1, a2);
}

static IRExpr* mkU64 ( ULong i )
{
   return IRExpr_Const(IRConst_U64(i));
}

static IRExpr* mkU32 ( UInt i )
{
   return IRExpr_Const(IRConst_U32(i));
}

static IRExpr* bind ( Int binder )
{
   return IRExpr_Binder(binder);
}




/*---------------------------------------------------------*/
/*--- ISelEnv                                           ---*/
/*---------------------------------------------------------*/

/* This carries around:

   - A mapping from IRTemp to IRType, giving the type of any IRTemp we
     might encounter.  This is computed before insn selection starts,
     and does not change.

   - A mapping from IRTemp to HReg.  This tells the insn selector
     which virtual register(s) are associated with each IRTemp
     temporary.  This is computed before insn selection starts, and
     does not change.  We expect this mapping to map precisely the
     same set of IRTemps as the type mapping does.

        - vregmap   holds the primary register for the IRTemp.
        - vregmapHI is only used for 64-bit integer-typed
             IRTemps.  It holds the identity of a second
             32-bit virtual HReg, which holds the high half
             of the value.

   - The code array, that is, the insns selected so far.

   - A counter, for generating new virtual registers.

   Note, this is all host-independent.  */

typedef
   struct {
      IRTypeEnv*   type_env;

      HReg*        vregmap;
      HReg*        vregmapHI;
      Int          n_vregmap;

      HInstrArray* code;

      Int          vreg_ctr;
   }
   ISelEnv;


static HReg lookupIRTemp ( ISelEnv* env, IRTemp tmp )
{
   vassert(tmp >= 0);
   vassert(tmp < env->n_vregmap);
   return env->vregmap[tmp];
}

static void lookupIRTemp64 ( HReg* vrHI, HReg* vrLO, ISelEnv* env, IRTemp tmp )
{
   vassert(tmp >= 0);
   vassert(tmp < env->n_vregmap);
   vassert(env->vregmapHI[tmp] != INVALID_HREG);
   *vrLO = env->vregmap[tmp];
   *vrHI = env->vregmapHI[tmp];
}

static void addInstr ( ISelEnv* env, X86Instr* instr )
{
   addHInstr(env->code, instr);
   if (vex_traceflags & VEX_TRACE_VCODE) {
      ppX86Instr(instr);
      vex_printf("\n");
   }
}

static HReg newVRegI ( ISelEnv* env )
{
   HReg reg = mkHReg(env->vreg_ctr, HRcInt, True/*virtual reg*/);
   env->vreg_ctr++;
   return reg;
}

static HReg newVRegF ( ISelEnv* env )
{
   HReg reg = mkHReg(env->vreg_ctr, HRcFloat, True/*virtual reg*/);
   env->vreg_ctr++;
   return reg;
}


/*---------------------------------------------------------*/
/*--- ISEL: Forward declarations                        ---*/
/*---------------------------------------------------------*/

/* These are organised as iselXXX and iselXXX_wrk pairs.  The
   iselXXX_wrk do the real work, but are not to be called directly.
   For each XXX, iselXXX calls its iselXXX_wrk counterpart, then
   checks that all returned registers are virtual. 
*/

static X86RMI*   iselIntExpr_RMI_wrk ( ISelEnv* env, IRExpr* e );
static X86RMI*   iselIntExpr_RMI     ( ISelEnv* env, IRExpr* e );

static X86RI*    iselIntExpr_RI_wrk ( ISelEnv* env, IRExpr* e );
static X86RI*    iselIntExpr_RI     ( ISelEnv* env, IRExpr* e );

static X86RM*    iselIntExpr_RM_wrk ( ISelEnv* env, IRExpr* e );
static X86RM*    iselIntExpr_RM     ( ISelEnv* env, IRExpr* e );

static HReg      iselIntExpr_R_wrk ( ISelEnv* env, IRExpr* e );
static HReg      iselIntExpr_R     ( ISelEnv* env, IRExpr* e );

static X86AMode* iselIntExpr_AMode_wrk ( ISelEnv* env, IRExpr* e );
static X86AMode* iselIntExpr_AMode     ( ISelEnv* env, IRExpr* e );

static void      iselIntExpr64_wrk ( HReg* rHi, HReg* rLo, 
                                     ISelEnv* env, IRExpr* e );
static void      iselIntExpr64     ( HReg* rHi, HReg* rLo, 
                                     ISelEnv* env, IRExpr* e );

static X86CondCode iselCondCode_wrk ( ISelEnv* env, IRExpr* e );
static X86CondCode iselCondCode     ( ISelEnv* env, IRExpr* e );

static HReg iselDblExpr ( ISelEnv* env, IRExpr* e );


/*---------------------------------------------------------*/
/*--- ISEL: Misc helpers                                ---*/
/*---------------------------------------------------------*/

/* Is this a 32-bit zero expression? */

static Bool isZero32 ( IRExpr* e )
{
   return e->tag == Iex_Const
          && e->Iex.Const.con->tag == Ico_U32
          && e->Iex.Const.con->Ico.U32 == 0;
}

/* Make a int reg-reg move. */

static X86Instr* mk_MOVsd_RR ( HReg src, HReg dst )
{
   vassert(hregClass(src) == HRcInt);
   vassert(hregClass(dst) == HRcInt);
   return X86Instr_Alu32R(Xalu_MOV, X86RMI_Reg(src), dst);
}


/* Given an amode, return one which references 4 bytes further
   along. */

static X86AMode* advance4 ( X86AMode* am )
{
   X86AMode* am4 = dopyX86AMode(am);
   /* Could also advance the _IR form, but no need yet. */
   vassert(am4->tag == Xam_IRRS);
   am4->Xam.IRRS.imm += 4;
   return am4;
}


/* Push an arg onto the host stack, in preparation for a call to a
   helper function of some kind.  Returns the number of 32-bit words
   pushed. */

static Int pushArg ( ISelEnv* env, IRExpr* arg )
{
   IRType arg_ty = typeOfIRExpr(env->type_env, arg);
   if (arg_ty == Ity_I32) {
      addInstr(env, X86Instr_Push(iselIntExpr_RMI(env, arg)));
      return 1;
   } else 
   if (arg_ty == Ity_I64) {
      HReg rHi, rLo;
      iselIntExpr64(&rHi, &rLo, env, arg);
      addInstr(env, X86Instr_Push(X86RMI_Reg(rHi)));
      addInstr(env, X86Instr_Push(X86RMI_Reg(rLo)));
      return 2;
   }
   ppIRExpr(arg);
   vpanic("pushArg(x86): can't handle arg of this type");
}


/* Complete the call to a helper function, by calling the 
   helper and clearing the args off the stack. */

static 
void callHelperAndClearArgs ( ISelEnv* env, X86CondCode cc, 
                              IRCallee* cee, Int n_arg_ws )
{
   /* Complication.  Need to decide which reg to use as the fn address
      pointer, in a way that doesn't trash regparm-passed
      parameters. */
   vassert(sizeof(void*) == 4);

   addInstr(env, X86Instr_Call( cc, (UInt)cee->addr, cee->regparms));
   if (n_arg_ws > 0)
      addInstr(env, X86Instr_Alu32R(Xalu_ADD,
                       X86RMI_Imm(4*n_arg_ws),
                       hregX86_ESP()));
}


/* Do a complete function call.  guard is a Ity_Bit expression
   indicating whether or not the call happens.  If guard==NULL, the
   call is unconditional. */

static
void doHelperCall ( ISelEnv* env, 
                    Bool passBBP, 
                    IRExpr* guard, IRCallee* cee, IRExpr** args )
{
   X86CondCode cc;
   HReg argregs[3];
   Int  not_done_yet, n_args, n_arg_ws, stack_limit, i, argreg;

   /* Marshal args for a call, do the call, and clear the stack.
      Complexities to consider:

      * if passBBP is True, %ebp (the baseblock pointer) is to be
        passed as the first arg.

      * If the callee claims regparmness of 1, 2 or 3, we must pass the
        first 1, 2 or 3 args in registers (EAX, EDX, and ECX
        respectively).  To keep things relatively simple, only args of
        type I32 may be passed as regparms -- just bomb out if anything
        else turns up.  Clearly this depends on the front ends not
        trying to pass any other types as regparms.  
   */

   vassert(cee->regparms >= 0 && cee->regparms <= 3);

   n_args = n_arg_ws = 0;
   while (args[n_args]) n_args++;

   not_done_yet = n_args;
   if (passBBP)
      not_done_yet++;

   stack_limit = cee->regparms;
   if (cee->regparms > 0 && passBBP) stack_limit--;

   /* Push (R to L) the stack-passed args, [n_args-1 .. stack_limit] */
   for (i = n_args-1; i >= stack_limit; i--) {
      n_arg_ws += pushArg(env, args[i]);
      not_done_yet--;
   }

   /* args [stack_limit-1 .. 0] and possibly %ebp are to be passed in
      registers. */

   if (cee->regparms > 0) {
      /* deal with regparms, not forgetting %ebp if needed. */
      argregs[0] = hregX86_EAX();
      argregs[1] = hregX86_EDX();
      argregs[2] = hregX86_ECX();
      argreg = cee->regparms;

      for (i = stack_limit-1; i >= 0; i--) {
         argreg--;
         vassert(argreg >= 0);
         vassert(typeOfIRExpr(env->type_env, args[i]) == Ity_I32);
         addInstr(env, X86Instr_Alu32R(Xalu_MOV, 
                                       iselIntExpr_RMI(env, args[i]),
                                       argregs[argreg]));
         not_done_yet--;
      }
      if (passBBP) {
         vassert(argreg == 1);
         addInstr(env, mk_MOVsd_RR( hregX86_EBP(), argregs[0]));
         not_done_yet--;
      }
   } else {
      /* No regparms.  Heave %ebp on the stack if needed. */
      if (passBBP) {
         addInstr(env, X86Instr_Push(X86RMI_Reg(hregX86_EBP())));
         n_arg_ws++;
         not_done_yet--;
      }
   }

   vassert(not_done_yet == 0);

   /* Now we can compute the condition.  We can't do it earlier
      because the argument computations could trash the condition
      codes.  Be a bit clever to handle the common case where the
      guard is 1:Bit. */
   cc = Xcc_ALWAYS;
   if (guard) {
      if (guard->tag == Iex_Const 
          && guard->Iex.Const.con->tag == Ico_Bit
          && guard->Iex.Const.con->Ico.Bit == True) {
         /* unconditional -- do nothing */
      } else {
         cc = iselCondCode( env, guard );
      }
   }

   /* call the helper, and get the args off the stack afterwards. */
   callHelperAndClearArgs( env, cc, cee, n_arg_ws );
}


/* Given a guest-state array descriptor, an index expression and a
   bias, generate an X86AMode holding the relevant guest state
   offset. */

static
X86AMode* genGuestArrayOffset ( ISelEnv* env, IRArray* descr, 
                                IRExpr* off, Int bias )
{
   Int elemSz = sizeofIRType(descr->elemTy);
   Int nElems = descr->nElems;

   /* throw out any cases not generated by an x86 front end.  In
      theory there might be a day where we need to handle them -- if
      we ever run non-x86-guest on x86 host. */

   if (nElems != 8 || (elemSz != 1 && elemSz != 8))
      vpanic("genGuestArrayOffset(x86 host)");

   /* Compute off into a reg, %off.  Then return:

         movl %off, %tmp
         addl $bias, %tmp  (if bias != 0)
         andl %tmp, 7
         ... base(%ebp, %tmp, shift) ...
   */
   HReg tmp  = newVRegI(env);
   HReg roff = iselIntExpr_R(env, off);
   addInstr(env, mk_MOVsd_RR(roff, tmp));
   if (bias != 0) {
      addInstr(env, 
               X86Instr_Alu32R(Xalu_ADD, X86RMI_Imm(bias), tmp));
   }
   addInstr(env, 
            X86Instr_Alu32R(Xalu_AND, X86RMI_Imm(7), tmp));
   vassert(elemSz == 1 || elemSz == 8);
   return
      X86AMode_IRRS( descr->base, hregX86_EBP(), tmp,
                                  elemSz==8 ? 3 : 0);
}


/*---------------------------------------------------------*/
/*--- ISEL: Integer expressions (32/16/8 bit)           ---*/
/*---------------------------------------------------------*/

/* Select insns for an integer-typed expression, and add them to the
   code list.  Return a reg holding the result.  This reg will be a
   virtual register.  THE RETURNED REG MUST NOT BE MODIFIED.  If you
   want to modify it, ask for a new vreg, copy it in there, and modify
   the copy.  The register allocator will do its best to map both
   vregs to the same real register, so the copies will often disappear
   later in the game.

   This should handle expressions of 32, 16 and 8-bit type.  All
   results are returned in a 32-bit register.  For 16- and 8-bit
   expressions, the upper 16/24 bits are arbitrary, so you should mask
   or sign extend partial values if necessary.
*/

static HReg iselIntExpr_R ( ISelEnv* env, IRExpr* e )
{
   HReg r = iselIntExpr_R_wrk(env, e);
   /* sanity checks ... */
#  if 0
   vex_printf("\n"); ppIRExpr(e); vex_printf("\n");
#  endif
   vassert(hregClass(r) == HRcInt);
   vassert(hregIsVirtual(r));
   return r;
}

/* DO NOT CALL THIS DIRECTLY ! */
static HReg iselIntExpr_R_wrk ( ISelEnv* env, IRExpr* e )
{
   MatchInfo mi;
   DECLARE_PATTERN(p_32to1_then_1Uto8);

   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32 || Ity_I16 || Ity_I8);

   switch (e->tag) {

   /* --------- TEMP --------- */
   case Iex_Tmp: {
      return lookupIRTemp(env, e->Iex.Tmp.tmp);
   }

   /* --------- LOAD --------- */
   case Iex_LDle: {
      HReg dst = newVRegI(env);
      X86AMode* amode = iselIntExpr_AMode ( env, e->Iex.LDle.addr );
      if (ty == Ity_I32) {
         addInstr(env, X86Instr_Alu32R(Xalu_MOV,
                                       X86RMI_Mem(amode), dst) );
         return dst;
      }
      if (ty == Ity_I16) {
         addInstr(env, X86Instr_LoadEX(2,False,amode,dst));
         return dst;
      }
      if (ty == Ity_I8) {
         addInstr(env, X86Instr_LoadEX(1,False,amode,dst));
         return dst;
      }
      break;
   }

   /* --------- BINARY OP --------- */
   case Iex_Binop: {
      X86AluOp   aluOp;
      X86ShiftOp shOp;

      /* Pattern: Sub32(0,x) */
      if (e->Iex.Binop.op == Iop_Sub32 && isZero32(e->Iex.Binop.arg1)) {
         HReg dst = newVRegI(env);
         HReg reg = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, mk_MOVsd_RR(reg,dst));
         addInstr(env, X86Instr_Unary32(Xun_NEG,X86RM_Reg(dst)));
         return dst;
      }

      /* Is it an addition or logical style op? */
      switch (e->Iex.Binop.op) {
         case Iop_Add8: case Iop_Add16: case Iop_Add32:
            aluOp = Xalu_ADD; break;
         case Iop_Sub8: case Iop_Sub16: case Iop_Sub32: 
            aluOp = Xalu_SUB; break;
         case Iop_And8: case Iop_And16: case Iop_And32: 
            aluOp = Xalu_AND; break;
         case Iop_Or8: case Iop_Or16: case Iop_Or32:  
            aluOp = Xalu_OR; break;
         case Iop_Xor8: case Iop_Xor16: case Iop_Xor32: 
            aluOp = Xalu_XOR; break;
         case Iop_Mul16: case Iop_Mul32: 
            aluOp = Xalu_MUL; break;
         default:
            aluOp = Xalu_INVALID; break;
      }
      /* For commutative ops we assume any literal
         values are on the second operand. */
      if (aluOp != Xalu_INVALID) {
         HReg dst    = newVRegI(env);
         HReg reg    = iselIntExpr_R(env, e->Iex.Binop.arg1);
         X86RMI* rmi = iselIntExpr_RMI(env, e->Iex.Binop.arg2);
         addInstr(env, mk_MOVsd_RR(reg,dst));
         addInstr(env, X86Instr_Alu32R(aluOp, rmi, dst));
         return dst;
      }
      /* Could do better here; forcing the first arg into a reg
         isn't always clever.
         -- t70 = Xor32(And32(Xor32(LDle:I32(Add32(t41,0xFFFFFFA0:I32)),
                        LDle:I32(Add32(t41,0xFFFFFFA4:I32))),LDle:I32(Add32(
                        t41,0xFFFFFFA8:I32))),LDle:I32(Add32(t41,0xFFFFFFA0:I32)))
            movl 0xFFFFFFA0(%vr41),%vr107
            movl 0xFFFFFFA4(%vr41),%vr108
            movl %vr107,%vr106
            xorl %vr108,%vr106
            movl 0xFFFFFFA8(%vr41),%vr109
            movl %vr106,%vr105
            andl %vr109,%vr105
            movl 0xFFFFFFA0(%vr41),%vr110
            movl %vr105,%vr104
            xorl %vr110,%vr104
            movl %vr104,%vr70
      */

      /* Perhaps a shift op? */
      switch (e->Iex.Binop.op) {
         case Iop_Shl32: case Iop_Shl16: case Iop_Shl8:
            shOp = Xsh_SHL; break;
         case Iop_Shr32: case Iop_Shr16: case Iop_Shr8: 
            shOp = Xsh_SHR; break;
         case Iop_Sar32: case Iop_Sar16: case Iop_Sar8: 
            shOp = Xsh_SAR; break;
         default:
            shOp = Xsh_INVALID; break;
      }
      if (shOp != Xsh_INVALID) {
         HReg dst = newVRegI(env);

         /* regL = the value to be shifted */
         HReg regL   = iselIntExpr_R(env, e->Iex.Binop.arg1);
         addInstr(env, mk_MOVsd_RR(regL,dst));

         /* Do any necessary widening for 16/8 bit operands */
         switch (e->Iex.Binop.op) {
            case Iop_Shr8:
               addInstr(env, X86Instr_Alu32R(
                                Xalu_AND, X86RMI_Imm(0xFF), dst));
               break;
            case Iop_Shr16:
               addInstr(env, X86Instr_Alu32R(
                                Xalu_AND, X86RMI_Imm(0xFFFF), dst));
               break;
            case Iop_Sar8:
               addInstr(env, X86Instr_Sh32(Xsh_SHL, 24, X86RM_Reg(dst)));
               addInstr(env, X86Instr_Sh32(Xsh_SAR, 24, X86RM_Reg(dst)));
               break;
            case Iop_Sar16:
               addInstr(env, X86Instr_Sh32(Xsh_SHL, 16, X86RM_Reg(dst)));
               addInstr(env, X86Instr_Sh32(Xsh_SAR, 16, X86RM_Reg(dst)));
               break;
            default: break;
         }

         /* Now consider the shift amount.  If it's a literal, we
            can do a much better job than the general case. */
         if (e->Iex.Binop.arg2->tag == Iex_Const) {
            /* assert that the IR is well-typed */
            Int nshift;
            vassert(e->Iex.Binop.arg2->Iex.Const.con->tag == Ico_U8);
            nshift = e->Iex.Binop.arg2->Iex.Const.con->Ico.U8;
	    vassert(nshift >= 0);
	    if (nshift > 0)
               /* Can't allow nshift==0 since that means %cl */
               addInstr(env, X86Instr_Sh32(
                                shOp, 
                                nshift,
                                X86RM_Reg(dst)));
         } else {
            /* General case; we have to force the amount into %cl. */
            HReg regR = iselIntExpr_R(env, e->Iex.Binop.arg2);
            addInstr(env, mk_MOVsd_RR(regR,hregX86_ECX()));
            addInstr(env, X86Instr_Sh32(shOp, 0/* %cl */, X86RM_Reg(dst)));
         }
         return dst;
      }

      /* Handle misc other ops. */
      if (e->Iex.Binop.op == Iop_8HLto16) {
         HReg hi8  = newVRegI(env);
         HReg lo8  = newVRegI(env);
         HReg hi8s = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg lo8s = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, mk_MOVsd_RR(hi8s, hi8));
         addInstr(env, mk_MOVsd_RR(lo8s, lo8));
         addInstr(env, X86Instr_Sh32(Xsh_SHL, 8, X86RM_Reg(hi8)));
         addInstr(env, X86Instr_Alu32R(Xalu_AND, X86RMI_Imm(0xFF), lo8));
         addInstr(env, X86Instr_Alu32R(Xalu_OR, X86RMI_Reg(lo8), hi8));
         return hi8;
      }

      if (e->Iex.Binop.op == Iop_16HLto32) {
         HReg hi16  = newVRegI(env);
         HReg lo16  = newVRegI(env);
         HReg hi16s = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg lo16s = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, mk_MOVsd_RR(hi16s, hi16));
         addInstr(env, mk_MOVsd_RR(lo16s, lo16));
         addInstr(env, X86Instr_Sh32(Xsh_SHL, 16, X86RM_Reg(hi16)));
         addInstr(env, X86Instr_Alu32R(Xalu_AND, X86RMI_Imm(0xFFFF), lo16));
         addInstr(env, X86Instr_Alu32R(Xalu_OR, X86RMI_Reg(lo16), hi16));
         return hi16;
      }

      if (e->Iex.Binop.op == Iop_MullS16 || e->Iex.Binop.op == Iop_MullS8
          || e->Iex.Binop.op == Iop_MullU16 || e->Iex.Binop.op == Iop_MullU8) {
         HReg a16   = newVRegI(env);
         HReg b16   = newVRegI(env);
         HReg a16s  = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg b16s  = iselIntExpr_R(env, e->Iex.Binop.arg2);
         Int  shift = (e->Iex.Binop.op == Iop_MullS8 
                       || e->Iex.Binop.op == Iop_MullU8)
                         ? 24 : 16;
         X86ShiftOp shr_op = (e->Iex.Binop.op == Iop_MullS8 
                              || e->Iex.Binop.op == Iop_MullS16)
                                ? Xsh_SAR : Xsh_SHR;

         addInstr(env, mk_MOVsd_RR(a16s, a16));
         addInstr(env, mk_MOVsd_RR(b16s, b16));
         addInstr(env, X86Instr_Sh32(Xsh_SHL, shift, X86RM_Reg(a16)));
         addInstr(env, X86Instr_Sh32(Xsh_SHL, shift, X86RM_Reg(b16)));
         addInstr(env, X86Instr_Sh32(shr_op,  shift, X86RM_Reg(a16)));
         addInstr(env, X86Instr_Sh32(shr_op,  shift, X86RM_Reg(b16)));
         addInstr(env, X86Instr_Alu32R(Xalu_MUL, X86RMI_Reg(a16), b16));
         return b16;
      }

      if (e->Iex.Binop.op == Iop_CmpF64) {
         HReg fL = iselDblExpr(env, e->Iex.Binop.arg1);
         HReg fR = iselDblExpr(env, e->Iex.Binop.arg2);
         HReg dst = newVRegI(env);
         addInstr(env, X86Instr_FpCmp(fL,fR,dst));
         /* shift this right 8 bits so as to conform to CmpF64
            definition. */
         addInstr(env, X86Instr_Sh32(Xsh_SHR, 8, X86RM_Reg(dst)));
         return dst;
      }

      if (e->Iex.Binop.op == Iop_F64toI32 || e->Iex.Binop.op == Iop_F64toI16) {
         Int  sz   = e->Iex.Binop.op == Iop_F64toI16 ? 2 : 4;
         HReg rf   = iselDblExpr(env, e->Iex.Binop.arg2);
         HReg rrm  = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg rrm2 = newVRegI(env);
         HReg dst  = newVRegI(env);

         /* Used several times ... */
         X86AMode* zero_esp = X86AMode_IR(0, hregX86_ESP());

	 /* rf now holds the value to be converted, and rrm holds the
	    rounding mode value, encoded as per the IRRoundingMode
	    enum.  The first thing to do is set the FPU's rounding
	    mode accordingly. */

         /* Create a space, both for the control word messing, and for
	    the actual store conversion. */
         /* subl $4, %esp */
         addInstr(env, 
                  X86Instr_Alu32R(Xalu_SUB, X86RMI_Imm(4), hregX86_ESP()));
	 /* movl %rrm, %rrm2
            andl $3, %rrm2   -- shouldn't be needed; paranoia
            shll $10, %rrm2
            orl  $0x037F, %rrm2
            movl %rrm2, 0(%esp)
            fldcw 0(%esp)
	 */
         addInstr(env, mk_MOVsd_RR(rrm, rrm2));
	 addInstr(env, X86Instr_Alu32R(Xalu_AND, X86RMI_Imm(3), rrm2));
	 addInstr(env, X86Instr_Sh32(Xsh_SHL, 10, X86RM_Reg(rrm2)));
	 addInstr(env, X86Instr_Alu32R(Xalu_OR, X86RMI_Imm(0x037F), rrm2));
	 addInstr(env, X86Instr_Alu32M(Xalu_MOV, X86RI_Reg(rrm2), zero_esp));
	 addInstr(env, X86Instr_FpLdStCW(True/*load*/, zero_esp));

         /* gistw/l %rf, 0(%esp) */
         addInstr(env, X86Instr_FpLdStI(False/*store*/, sz, rf, zero_esp));

         if (sz == 2) {
            /* movzwl 0(%esp), %dst */
            addInstr(env, X86Instr_LoadEX(2,False,zero_esp,dst));
        } else {
            /* movl 0(%esp), %dst */
            vassert(sz == 4);
            addInstr(env, X86Instr_Alu32R(
                             Xalu_MOV, X86RMI_Mem(zero_esp), dst));
         }

	 /* Restore default FPU control.
            movl $0x037F, 0(%esp)
            fldcw 0(%esp)
	 */
         addInstr(env, X86Instr_Alu32M(Xalu_MOV, X86RI_Imm(0x037F), zero_esp));
	 addInstr(env, X86Instr_FpLdStCW(True/*load*/, zero_esp));

         /* addl $4, %esp */
         addInstr(env, 
                  X86Instr_Alu32R(Xalu_ADD, X86RMI_Imm(4), hregX86_ESP()));
         return dst;
      }

      /* C3210 flags following FPU partial remainder (fprem), both
         IEEE compliant (PREM1) and non-IEEE compliant (PREM). */
      if (e->Iex.Binop.op == Iop_PRemC3210F64
          || e->Iex.Binop.op == Iop_PRem1C3210F64) {
         HReg junk = newVRegF(env);
         HReg dst  = newVRegI(env);
         HReg srcL = iselDblExpr(env, e->Iex.Binop.arg1);
         HReg srcR = iselDblExpr(env, e->Iex.Binop.arg2);
         addInstr(env, X86Instr_FpBinary(
                           e->Iex.Binop.op==Iop_PRemC3210F64 ? Xfp_PREM : Xfp_PREM1,
                           srcL,srcR,junk
                 ));
         /* The previous pseudo-insn will have left the FPU's C3210
            flags set correctly.  So bag them. */
         addInstr(env, X86Instr_FpStSW_AX());
         addInstr(env, mk_MOVsd_RR(hregX86_EAX(), dst));
	 addInstr(env, X86Instr_Alu32R(Xalu_AND, X86RMI_Imm(0x4700), dst));
         return dst;
      }

      break;
   }

   /* --------- UNARY OP --------- */
   case Iex_Unop: {
      /* 1Uto8(32to1(expr32)) */
      DEFINE_PATTERN(p_32to1_then_1Uto8,
                     unop(Iop_1Uto8,unop(Iop_32to1,bind(0))));
      if (matchIRExpr(&mi,p_32to1_then_1Uto8,e)) {
         IRExpr* expr32 = mi.bindee[0];
         HReg dst = newVRegI(env);
         HReg src = iselIntExpr_R(env, expr32);
         addInstr(env, mk_MOVsd_RR(src,dst) );
         addInstr(env, X86Instr_Alu32R(Xalu_AND,
                                       X86RMI_Imm(1), dst));
         return dst;
      }

      /* 16Uto32(LDle(expr32)) */
      {
         DECLARE_PATTERN(p_LDle16_then_16Uto32);
         DEFINE_PATTERN(p_LDle16_then_16Uto32,
            unop(Iop_16Uto32,IRExpr_LDle(Ity_I16,bind(0))) );
         if (matchIRExpr(&mi,p_LDle16_then_16Uto32,e)) {
            HReg dst = newVRegI(env);
            X86AMode* amode = iselIntExpr_AMode ( env, mi.bindee[0] );
            addInstr(env, X86Instr_LoadEX(2,False,amode,dst));
            return dst;
         }
      }

      switch (e->Iex.Unop.op) {
         case Iop_8Uto16:
         case Iop_8Uto32:
         case Iop_16Uto32: {
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            UInt mask = e->Iex.Unop.op==Iop_16Uto32 ? 0xFFFF : 0xFF;
            addInstr(env, mk_MOVsd_RR(src,dst) );
            addInstr(env, X86Instr_Alu32R(Xalu_AND,
                                          X86RMI_Imm(mask), dst));
            return dst;
         }
         case Iop_8Sto16:
         case Iop_8Sto32:
         case Iop_16Sto32: {
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            UInt amt = e->Iex.Unop.op==Iop_16Sto32 ? 16 : 24;
            addInstr(env, mk_MOVsd_RR(src,dst) );
            addInstr(env, X86Instr_Sh32(Xsh_SHL, amt, X86RM_Reg(dst)));
            addInstr(env, X86Instr_Sh32(Xsh_SAR, amt, X86RM_Reg(dst)));
            return dst;
         }
	 case Iop_Not8:
	 case Iop_Not16:
         case Iop_Not32: {
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, mk_MOVsd_RR(src,dst) );
            addInstr(env, X86Instr_Unary32(Xun_NOT,X86RM_Reg(dst)));
            return dst;
         }
         case Iop_64HIto32: {
            HReg rHi, rLo;
            iselIntExpr64(&rHi,&rLo, env, e->Iex.Unop.arg);
            return rHi; /* and abandon rLo .. poor wee thing :-) */
         }
         case Iop_64to32: {
            HReg rHi, rLo;
            iselIntExpr64(&rHi,&rLo, env, e->Iex.Unop.arg);
            return rLo; /* similar stupid comment to the above ... */
         }
         case Iop_16HIto8:
         case Iop_32HIto16: {
            HReg dst  = newVRegI(env);
            HReg src  = iselIntExpr_R(env, e->Iex.Unop.arg);
            Int shift = e->Iex.Unop.op == Iop_16HIto8 ? 8 : 16;
            addInstr(env, mk_MOVsd_RR(src,dst) );
            addInstr(env, X86Instr_Sh32(Xsh_SHR, shift, X86RM_Reg(dst)));
            return dst;
         }
         case Iop_1Uto32:
         case Iop_1Uto8: {
            HReg dst         = newVRegI(env);
            X86CondCode cond = iselCondCode(env, e->Iex.Unop.arg);
            addInstr(env, X86Instr_Set32(cond,dst));
            return dst;
         }
         case Iop_1Sto8:
         case Iop_1Sto16:
         case Iop_1Sto32: {
            /* could do better than this, but for now ... */
            HReg dst         = newVRegI(env);
            X86CondCode cond = iselCondCode(env, e->Iex.Unop.arg);
            addInstr(env, X86Instr_Set32(cond,dst));
            addInstr(env, X86Instr_Sh32(Xsh_SHL, 31, X86RM_Reg(dst)));
            addInstr(env, X86Instr_Sh32(Xsh_SAR, 31, X86RM_Reg(dst)));
            return dst;
         }
         case Iop_Ctz32: {
            /* Count trailing zeroes, implemented by x86 'bsfl' */
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, X86Instr_Bsfr32(True,src,dst));
            return dst;
         }
         case Iop_Clz32: {
            /* Count leading zeroes.  Do 'bsrl' to establish the index
               of the highest set bit, and subtract that value from
               31. */
            HReg tmp = newVRegI(env);
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, X86Instr_Bsfr32(False,src,tmp));
            addInstr(env, X86Instr_Alu32R(Xalu_MOV, 
                                          X86RMI_Imm(31), dst));
            addInstr(env, X86Instr_Alu32R(Xalu_SUB,
                                          X86RMI_Reg(tmp), dst));
            return dst;
         }

         case Iop_16to8:
         case Iop_32to8:
         case Iop_32to16:
            /* These are no-ops. */
            return iselIntExpr_R(env, e->Iex.Unop.arg);

         default: 
            break;
      }
      break;
   }

   /* --------- GET --------- */
   case Iex_Get: {
      if (ty == Ity_I32) {
         HReg dst = newVRegI(env);
         addInstr(env, X86Instr_Alu32R(
                          Xalu_MOV, 
                          X86RMI_Mem(X86AMode_IR(e->Iex.Get.offset,
                                                 hregX86_EBP())),
                          dst));
         return dst;
      }
      if (ty == Ity_I8 || ty == Ity_I16) {
         HReg dst = newVRegI(env);
         addInstr(env, X86Instr_LoadEX(
                          ty==Ity_I8 ? 1 : 2,
                          False,
                          X86AMode_IR(e->Iex.Get.offset,hregX86_EBP()),
                          dst));
         return dst;
      }
      break;
   }

   case Iex_GetI: {
      X86AMode* am 
         = genGuestArrayOffset(
              env, e->Iex.GetI.descr, 
                   e->Iex.GetI.ix, e->Iex.GetI.bias );
      HReg dst = newVRegI(env);
      if (ty == Ity_I8) {
         addInstr(env, X86Instr_LoadEX( 1, False, am, dst ));
         return dst;
      }
      break;
   }

   /* --------- CCALL --------- */
   case Iex_CCall: {
      HReg    dst = newVRegI(env);
      vassert(ty == Ity_I32);

      /* be very restrictive for now.  Only 32/64-bit ints allowed
         for args, and 32 bits for return type. */
      if (e->Iex.CCall.retty != Ity_I32)
         goto irreducible;

      /* Marshal args, do the call, clear stack. */
      doHelperCall( env, False, NULL, e->Iex.CCall.cee, e->Iex.CCall.args );

      addInstr(env, mk_MOVsd_RR(hregX86_EAX(), dst));
      return dst;
   }

   /* --------- LITERAL --------- */
   /* 32/16/8-bit literals */
   case Iex_Const: {
      X86RMI* rmi = iselIntExpr_RMI ( env, e );
      HReg    r   = newVRegI(env);
      addInstr(env, X86Instr_Alu32R(Xalu_MOV, rmi, r));
      return r;
   }

   /* --------- MULTIPLEX --------- */
   case Iex_Mux0X: {
     if ((ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8)
         && typeOfIRExpr(env->type_env,e->Iex.Mux0X.cond) == Ity_I8) {
        HReg r8;
        HReg rX   = iselIntExpr_R(env, e->Iex.Mux0X.exprX);
        X86RM* r0 = iselIntExpr_RM(env, e->Iex.Mux0X.expr0);
        HReg dst = newVRegI(env);
        addInstr(env, mk_MOVsd_RR(rX,dst));
        r8 = iselIntExpr_R(env, e->Iex.Mux0X.cond);
        addInstr(env, X86Instr_Test32(X86RI_Imm(0xFF), X86RM_Reg(r8)));
        addInstr(env, X86Instr_CMov32(Xcc_Z,r0,dst));
        return dst;
      }
      break;
   }

   default: 
   break;
   } /* switch (e->tag) */

   /* We get here if no pattern matched. */
  irreducible:
   ppIRExpr(e);
   vpanic("iselIntExpr_R: cannot reduce tree");
}


/*---------------------------------------------------------*/
/*--- ISEL: Integer expression auxiliaries              ---*/
/*---------------------------------------------------------*/

/* --------------------- AMODEs --------------------- */

/* Return an AMode which computes the value of the specified
   expression, possibly also adding insns to the code list as a
   result.  The expression may only be a 32-bit one.
*/

static Bool sane_AMode ( X86AMode* am )
{
   switch (am->tag) {
      case Xam_IR:
         return hregClass(am->Xam.IR.reg) == HRcInt
                && (hregIsVirtual(am->Xam.IR.reg)
                    || am->Xam.IR.reg == hregX86_EBP());
      case Xam_IRRS:
         return hregClass(am->Xam.IRRS.base) == HRcInt
                && hregIsVirtual(am->Xam.IRRS.base)
                && hregClass(am->Xam.IRRS.index) == HRcInt
                && hregIsVirtual(am->Xam.IRRS.index);
      default:
        vpanic("sane_AMode: unknown x86 amode tag");
   }
}

static X86AMode* iselIntExpr_AMode ( ISelEnv* env, IRExpr* e )
{
   X86AMode* am = iselIntExpr_AMode_wrk(env, e);
   vassert(sane_AMode(am));
   return am;
}

/* DO NOT CALL THIS DIRECTLY ! */
static X86AMode* iselIntExpr_AMode_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32);

   /* Add32(expr1, Shl32(expr2, imm)) */
   if (e->tag == Iex_Binop
       && e->Iex.Binop.op == Iop_Add32
       && e->Iex.Binop.arg2->tag == Iex_Binop
       && e->Iex.Binop.arg2->Iex.Binop.op == Iop_Shl32
       && e->Iex.Binop.arg2->Iex.Binop.arg2->tag == Iex_Const
       && e->Iex.Binop.arg2->Iex.Binop.arg2->Iex.Const.con->tag == Ico_U8) {
      UInt shift = e->Iex.Binop.arg2->Iex.Binop.arg2->Iex.Const.con->Ico.U8;
      if (shift == 1 || shift == 2 || shift == 3) {
         HReg r1 = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg r2 = iselIntExpr_R(env, e->Iex.Binop.arg2->Iex.Binop.arg1 );
         return X86AMode_IRRS(0, r1, r2, shift);
      }
   }

   /* Add32(expr,i) */
   if (e->tag == Iex_Binop 
       && e->Iex.Binop.op == Iop_Add32
       && e->Iex.Binop.arg2->tag == Iex_Const
       && e->Iex.Binop.arg2->Iex.Const.con->tag == Ico_U32) {
      HReg r1 = iselIntExpr_R(env,  e->Iex.Binop.arg1);
      return X86AMode_IR(e->Iex.Binop.arg2->Iex.Const.con->Ico.U32, r1);
   }

   /* Doesn't match anything in particular.  Generate it into
      a register and use that. */
   {
      HReg r1 = iselIntExpr_R(env, e);
      return X86AMode_IR(0, r1);
   }
}


/* --------------------- RMIs --------------------- */

/* Similarly, calculate an expression into an X86RMI operand.  As with
   iselIntExpr_R, the expression can have type 32, 16 or 8 bits.  */

static X86RMI* iselIntExpr_RMI ( ISelEnv* env, IRExpr* e )
{
   X86RMI* rmi = iselIntExpr_RMI_wrk(env, e);
   /* sanity checks ... */
   switch (rmi->tag) {
      case Xrmi_Imm:
         return rmi;
      case Xrmi_Reg:
         vassert(hregClass(rmi->Xrmi.Reg.reg) == HRcInt);
         vassert(hregIsVirtual(rmi->Xrmi.Reg.reg));
         return rmi;
      case Xrmi_Mem:
         vassert(sane_AMode(rmi->Xrmi.Mem.am));
         return rmi;
      default:
         vpanic("iselIntExpr_RMI: unknown x86 RMI tag");
   }
}

/* DO NOT CALL THIS DIRECTLY ! */
static X86RMI* iselIntExpr_RMI_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8);

   /* special case: immediate */
   if (e->tag == Iex_Const) {
      UInt u;
      switch (e->Iex.Const.con->tag) {
         case Ico_U32: u = e->Iex.Const.con->Ico.U32; break;
         case Ico_U16: u = 0xFFFF & (e->Iex.Const.con->Ico.U16); break;
         case Ico_U8:  u = 0xFF   & (e->Iex.Const.con->Ico.U8); break;
         default: vpanic("iselIntExpr_RMI.Iex_Const(x86h)");
      }
      return X86RMI_Imm(u);
   }

   /* special case: 32-bit GET */
   if (e->tag == Iex_Get && ty == Ity_I32) {
      return X86RMI_Mem(X86AMode_IR(e->Iex.Get.offset,
                                    hregX86_EBP()));
   }

   /* special case: load from memory */

   /* default case: calculate into a register and return that */
   {
      HReg r = iselIntExpr_R ( env, e );
      return X86RMI_Reg(r);
   }
}


/* --------------------- RIs --------------------- */

/* Calculate an expression into an X86RI operand.  As with
   iselIntExpr_R, the expression can have type 32, 16 or 8 bits. */

static X86RI* iselIntExpr_RI ( ISelEnv* env, IRExpr* e )
{
   X86RI* ri = iselIntExpr_RI_wrk(env, e);
   /* sanity checks ... */
   switch (ri->tag) {
      case Xri_Imm:
         return ri;
      case Xrmi_Reg:
         vassert(hregClass(ri->Xri.Reg.reg) == HRcInt);
         vassert(hregIsVirtual(ri->Xri.Reg.reg));
         return ri;
      default:
         vpanic("iselIntExpr_RI: unknown x86 RI tag");
   }
}

/* DO NOT CALL THIS DIRECTLY ! */
static X86RI* iselIntExpr_RI_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8);

   /* special case: immediate */
   if (e->tag == Iex_Const) {
      UInt u;
      switch (e->Iex.Const.con->tag) {
         case Ico_U32: u = e->Iex.Const.con->Ico.U32; break;
         case Ico_U16: u = 0xFFFF & (e->Iex.Const.con->Ico.U16); break;
         case Ico_U8:  u = 0xFF   & (e->Iex.Const.con->Ico.U8); break;
         default: vpanic("iselIntExpr_RMI.Iex_Const(x86h)");
      }
      return X86RI_Imm(u);
   }

   /* default case: calculate into a register and return that */
   {
      HReg r = iselIntExpr_R ( env, e );
      return X86RI_Reg(r);
   }
}


/* --------------------- RMs --------------------- */

/* Similarly, calculate an expression into an X86RM operand.  As with
   iselIntExpr_R, the expression can have type 32, 16 or 8 bits.  */

static X86RM* iselIntExpr_RM ( ISelEnv* env, IRExpr* e )
{
   X86RM* rm = iselIntExpr_RM_wrk(env, e);
   /* sanity checks ... */
   switch (rm->tag) {
      case Xrm_Reg:
         vassert(hregClass(rm->Xrm.Reg.reg) == HRcInt);
         vassert(hregIsVirtual(rm->Xrm.Reg.reg));
         return rm;
      case Xrm_Mem:
         vassert(sane_AMode(rm->Xrm.Mem.am));
         return rm;
      default:
         vpanic("iselIntExpr_RM: unknown x86 RM tag");
   }
}

/* DO NOT CALL THIS DIRECTLY ! */
static X86RM* iselIntExpr_RM_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8);

   /* special case: 32-bit GET */
   if (e->tag == Iex_Get && ty == Ity_I32) {
      return X86RM_Mem(X86AMode_IR(e->Iex.Get.offset,
                                   hregX86_EBP()));
   }

   /* special case: load from memory */

   /* default case: calculate into a register and return that */
   {
      HReg r = iselIntExpr_R ( env, e );
      return X86RM_Reg(r);
   }
}


/* --------------------- CONDCODE --------------------- */

/* Generate code to evaluated a bit-typed expression, returning the
   condition code which would correspond when the expression would
   notionally have returned 1. */

static X86CondCode iselCondCode ( ISelEnv* env, IRExpr* e )
{
   /* Uh, there's nothing we can sanity check here, unfortunately. */
   return iselCondCode_wrk(env,e);
}

/* DO NOT CALL THIS DIRECTLY ! */
static X86CondCode iselCondCode_wrk ( ISelEnv* env, IRExpr* e )
{
   MatchInfo mi;
   DECLARE_PATTERN(p_32to1);
   //DECLARE_PATTERN(p_eq32_literal);
   //DECLARE_PATTERN(p_ne32_zero);
   DECLARE_PATTERN(p_1Uto32_then_32to1);

   vassert(e);
   vassert(typeOfIRExpr(env->type_env,e) == Ity_Bit);

   /* Constant 1:Bit */
   if (e->tag == Iex_Const && e->Iex.Const.con->Ico.Bit == True) {
      vassert(e->Iex.Const.con->tag == Ico_Bit);
      HReg r = newVRegI(env);
      addInstr(env, X86Instr_Alu32R(Xalu_MOV,X86RMI_Imm(0),r));
      addInstr(env, X86Instr_Alu32R(Xalu_XOR,X86RMI_Reg(r),r));
      return Xcc_Z;
   }

   /* Not1(...) */
   if (e->tag == Iex_Unop && e->Iex.Unop.op == Iop_Not1) {
      /* Generate code for the arg, and negate the test condition */
      return 1 ^ iselCondCode(env, e->Iex.Unop.arg);
   }

   /* 32to1(1Uto32(expr1)) -- the casts are pointless, ignore them */
   DEFINE_PATTERN(p_1Uto32_then_32to1,
                  unop(Iop_32to1,unop(Iop_1Uto32,bind(0))));
   if (matchIRExpr(&mi,p_1Uto32_then_32to1,e)) {
      IRExpr* expr1 = mi.bindee[0];
      return iselCondCode(env, expr1);
   }

   /* pattern: 32to1(expr32) */
   DEFINE_PATTERN(p_32to1, 
      unop(Iop_32to1,bind(0))
   );
   if (matchIRExpr(&mi,p_32to1,e)) {
      X86RM* rm = iselIntExpr_RM(env, mi.bindee[0]);
      addInstr(env, X86Instr_Test32(X86RI_Imm(1),rm));
      return Xcc_NZ;
   }

   /* CmpEQ8 / CmpNE8 */
   if (e->tag == Iex_Binop 
       && (e->Iex.Binop.op == Iop_CmpEQ8
           || e->Iex.Binop.op == Iop_CmpNE8)) {
      HReg    r1   = iselIntExpr_R(env, e->Iex.Binop.arg1);
      X86RMI* rmi2 = iselIntExpr_RMI(env, e->Iex.Binop.arg2);
      HReg    r    = newVRegI(env);
      addInstr(env, mk_MOVsd_RR(r1,r));
      addInstr(env, X86Instr_Alu32R(Xalu_XOR,rmi2,r));
      addInstr(env, X86Instr_Alu32R(Xalu_AND,X86RMI_Imm(0xFF),r));
      switch (e->Iex.Binop.op) {
         case Iop_CmpEQ8:  return Xcc_Z;
         case Iop_CmpNE8:  return Xcc_NZ;
         default: vpanic("iselCondCode(x86): CmpXX8");
      }
   }

   /* CmpEQ16 / CmpNE16 */
   if (e->tag == Iex_Binop 
       && (e->Iex.Binop.op == Iop_CmpEQ16
           || e->Iex.Binop.op == Iop_CmpNE16)) {
      HReg    r1   = iselIntExpr_R(env, e->Iex.Binop.arg1);
      X86RMI* rmi2 = iselIntExpr_RMI(env, e->Iex.Binop.arg2);
      HReg    r    = newVRegI(env);
      addInstr(env, mk_MOVsd_RR(r1,r));
      addInstr(env, X86Instr_Alu32R(Xalu_XOR,rmi2,r));
      addInstr(env, X86Instr_Alu32R(Xalu_AND,X86RMI_Imm(0xFFFF),r));
      switch (e->Iex.Binop.op) {
         case Iop_CmpEQ16:  return Xcc_Z;
         case Iop_CmpNE16:  return Xcc_NZ;
         default: vpanic("iselCondCode(x86): CmpXX16");
      }
   }

   /* CmpNE32(1Sto32(b), 0) ==> b */
   {
      DECLARE_PATTERN(p_CmpNE32_1Sto32);
      DEFINE_PATTERN(
         p_CmpNE32_1Sto32,
         binop(Iop_CmpNE32, unop(Iop_1Sto32,bind(0)), mkU32(0)));
      if (matchIRExpr(&mi, p_CmpNE32_1Sto32, e)) {
         return iselCondCode(env, mi.bindee[0]);
      }
   }

   /* Cmp*32*(x,y) */
   if (e->tag == Iex_Binop 
       && (e->Iex.Binop.op == Iop_CmpEQ32
           || e->Iex.Binop.op == Iop_CmpNE32
           || e->Iex.Binop.op == Iop_CmpLT32S
           || e->Iex.Binop.op == Iop_CmpLT32U
           || e->Iex.Binop.op == Iop_CmpLE32S
           || e->Iex.Binop.op == Iop_CmpLE32U)) {
      HReg    r1   = iselIntExpr_R(env, e->Iex.Binop.arg1);
      X86RMI* rmi2 = iselIntExpr_RMI(env, e->Iex.Binop.arg2);
      addInstr(env, X86Instr_Alu32R(Xalu_CMP,rmi2,r1));
      switch (e->Iex.Binop.op) {
         case Iop_CmpEQ32:  return Xcc_Z;
         case Iop_CmpNE32:  return Xcc_NZ;
         case Iop_CmpLT32S: return Xcc_L;
         case Iop_CmpLT32U: return Xcc_B;
         case Iop_CmpLE32S: return Xcc_LE;
         case Iop_CmpLE32U: return Xcc_BE;
         default: vpanic("iselCondCode(x86): CmpXX32");
      }
   }

   /* CmpNE64(1Sto64(b), 0) ==> b */
   {
      DECLARE_PATTERN(p_CmpNE64_1Sto64);
      DEFINE_PATTERN(
         p_CmpNE64_1Sto64,
         binop(Iop_CmpNE64, unop(Iop_1Sto64,bind(0)), mkU64(0)));
      if (matchIRExpr(&mi, p_CmpNE64_1Sto64, e)) {
         return iselCondCode(env, mi.bindee[0]);
      }
   }

   /* CmpNE64 */
   if (e->tag == Iex_Binop 
       && e->Iex.Binop.op == Iop_CmpNE64) {
      HReg hi1, hi2, lo1, lo2;
      HReg tHi = newVRegI(env);
      HReg tLo = newVRegI(env);
      iselIntExpr64( &hi1, &lo1, env, e->Iex.Binop.arg1 );
      iselIntExpr64( &hi2, &lo2, env, e->Iex.Binop.arg2 );
      addInstr(env, mk_MOVsd_RR(hi1, tHi));
      addInstr(env, X86Instr_Alu32R(Xalu_XOR,X86RMI_Reg(hi2), tHi));
      addInstr(env, mk_MOVsd_RR(lo1, tLo));
      addInstr(env, X86Instr_Alu32R(Xalu_XOR,X86RMI_Reg(lo2), tLo));
      addInstr(env, X86Instr_Alu32R(Xalu_OR,X86RMI_Reg(tHi), tLo));
      switch (e->Iex.Binop.op) {
         case Iop_CmpNE64:  return Xcc_NZ;
         default: vpanic("iselCondCode(x86): CmpXX64");
      }
   }

   /* var */
   if (e->tag == Iex_Tmp) {
      HReg r32 = lookupIRTemp(env, e->Iex.Tmp.tmp);
      HReg dst = newVRegI(env);
      addInstr(env, mk_MOVsd_RR(r32,dst));
      addInstr(env, X86Instr_Alu32R(Xalu_AND,X86RMI_Imm(1),dst));
      return Xcc_NZ;
   }

   ppIRExpr(e);
   vpanic("iselCondCode");
}


/*---------------------------------------------------------*/
/*--- ISEL: Integer expressions (64 bit)                ---*/
/*---------------------------------------------------------*/

/* Compute a 64-bit value into a register pair, which is returned as
   the first two parameters.  As with iselIntExpr_R, these may be
   either real or virtual regs; in any case they must not be changed
   by subsequent code emitted by the caller.  */

static void iselIntExpr64 ( HReg* rHi, HReg* rLo, ISelEnv* env, IRExpr* e )
{
   iselIntExpr64_wrk(rHi, rLo, env, e);
#  if 0
   vex_printf("\n"); ppIRExpr(e); vex_printf("\n");
#  endif
   vassert(hregClass(*rHi) == HRcInt);
   vassert(hregIsVirtual(*rHi));
   vassert(hregClass(*rLo) == HRcInt);
   vassert(hregIsVirtual(*rLo));
}

/* DO NOT CALL THIS DIRECTLY ! */
static void iselIntExpr64_wrk ( HReg* rHi, HReg* rLo, ISelEnv* env, IRExpr* e )
{
  //   MatchInfo mi;
   vassert(e);
   vassert(typeOfIRExpr(env->type_env,e) == Ity_I64);

   if (e->tag == Iex_Const) {
      ULong w64 = e->Iex.Const.con->Ico.U64;
      UInt  wHi = ((UInt)(w64 >> 32)) & 0xFFFFFFFF;
      UInt  wLo = ((UInt)w64) & 0xFFFFFFFF;
      HReg  tLo = newVRegI(env);
      HReg  tHi = newVRegI(env);
      vassert(e->Iex.Const.con->tag == Ico_U64);
      addInstr(env, X86Instr_Alu32R(Xalu_MOV, X86RMI_Imm(wHi), tHi));
      addInstr(env, X86Instr_Alu32R(Xalu_MOV, X86RMI_Imm(wLo), tLo));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* read 64-bit IRTemp */
   if (e->tag == Iex_Tmp) {
      lookupIRTemp64( rHi, rLo, env, e->Iex.Tmp.tmp);
      return;
   }

   /* 64-bit load */
   if (e->tag == Iex_LDle) {
      /* It would be better to generate the address into an amode and
         then do advance4 to get the hi-half address. */
      vassert(e->Iex.LDle.ty == Ity_I64);
      HReg tLo = newVRegI(env);
      HReg tHi = newVRegI(env);
      HReg rA = iselIntExpr_R(env, e->Iex.LDle.addr);
      addInstr(env, X86Instr_Alu32R(
                        Xalu_MOV,
                        X86RMI_Mem(X86AMode_IR(0, rA)), tLo));
      addInstr(env, X86Instr_Alu32R(
                        Xalu_MOV,
                        X86RMI_Mem(X86AMode_IR(4, rA)), tHi));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* 64-bit GETI */
   if (e->tag == Iex_GetI) {
      X86AMode* am 
         = genGuestArrayOffset( env, e->Iex.GetI.descr, 
                                     e->Iex.GetI.ix, e->Iex.GetI.bias );
      X86AMode* am4 = advance4(am);
      HReg tLo = newVRegI(env);
      HReg tHi = newVRegI(env);
      addInstr(env, X86Instr_Alu32R( Xalu_MOV, X86RMI_Mem(am), tLo ));
      addInstr(env, X86Instr_Alu32R( Xalu_MOV, X86RMI_Mem(am4), tHi ));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* 64-bit Mux0X */
   if (e->tag == Iex_Mux0X) {
      HReg e0Lo, e0Hi, eXLo, eXHi, r8;
      HReg tLo = newVRegI(env);
      HReg tHi = newVRegI(env);
      iselIntExpr64(&e0Hi, &e0Lo, env, e->Iex.Mux0X.expr0);
      iselIntExpr64(&eXHi, &eXLo, env, e->Iex.Mux0X.exprX);
      addInstr(env, mk_MOVsd_RR(eXHi, tHi));
      addInstr(env, mk_MOVsd_RR(eXLo, tLo));
      r8 = iselIntExpr_R(env, e->Iex.Mux0X.cond);
      addInstr(env, X86Instr_Test32(X86RI_Imm(0xFF), X86RM_Reg(r8)));
      /* This assumes the first cmov32 doesn't trash the condition
         codes, so they are still available for the second cmov32 */
      addInstr(env, X86Instr_CMov32(Xcc_Z,X86RM_Reg(e0Hi),tHi));
      addInstr(env, X86Instr_CMov32(Xcc_Z,X86RM_Reg(e0Lo),tLo));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* 32 x 32 -> 64 multiply */
   if (e->tag == Iex_Binop
       && (e->Iex.Binop.op == Iop_MullU32
           || e->Iex.Binop.op == Iop_MullS32)) {
      /* get one operand into %eax, and the other into a R/M.  Need to
         make an educated guess about which is better in which. */
      HReg   tLo    = newVRegI(env);
      HReg   tHi    = newVRegI(env);
      Bool   syned  = e->Iex.Binop.op == Iop_MullS32;
      X86RM* rmLeft = iselIntExpr_RM(env, e->Iex.Binop.arg1);
      HReg   rRight = iselIntExpr_R(env, e->Iex.Binop.arg2);
      addInstr(env, mk_MOVsd_RR(rRight, hregX86_EAX()));
      addInstr(env, X86Instr_MulL(syned, Xss_32, rmLeft));
      /* Result is now in EDX:EAX.  Tell the caller. */
      addInstr(env, mk_MOVsd_RR(hregX86_EDX(), tHi));
      addInstr(env, mk_MOVsd_RR(hregX86_EAX(), tLo));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* 64 x 32 -> (32(rem),32(div)) division */
   if (e->tag == Iex_Binop
      && (e->Iex.Binop.op == Iop_DivModU64to32
          || e->Iex.Binop.op == Iop_DivModS64to32)) {
      /* Get the 64-bit operand into edx:eax, and the other
         into any old R/M. */
      HReg sHi, sLo;
      HReg   tLo     = newVRegI(env);
      HReg   tHi     = newVRegI(env);
      Bool   syned   = e->Iex.Binop.op == Iop_DivModS64to32;
      X86RM* rmRight = iselIntExpr_RM(env, e->Iex.Binop.arg2);
      iselIntExpr64(&sHi,&sLo, env, e->Iex.Binop.arg1);
      addInstr(env, mk_MOVsd_RR(sHi, hregX86_EDX()));
      addInstr(env, mk_MOVsd_RR(sLo, hregX86_EAX()));
      addInstr(env, X86Instr_Div(syned, Xss_32, rmRight));
      addInstr(env, mk_MOVsd_RR(hregX86_EDX(), tHi));
      addInstr(env, mk_MOVsd_RR(hregX86_EAX(), tLo));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* Iop_Or64 */
   if (e->tag == Iex_Binop
       && (e->Iex.Binop.op == Iop_Or64)) {
      HReg xLo, xHi, yLo, yHi;
      HReg tLo = newVRegI(env);
      HReg tHi = newVRegI(env);
      iselIntExpr64(&xHi, &xLo, env, e->Iex.Binop.arg1);
      addInstr(env, mk_MOVsd_RR(xHi, tHi));
      addInstr(env, mk_MOVsd_RR(xLo, tLo));
      iselIntExpr64(&yHi, &yLo, env, e->Iex.Binop.arg2);
      addInstr(env, X86Instr_Alu32R(Xalu_OR, X86RMI_Reg(yHi), tHi));
      addInstr(env, X86Instr_Alu32R(Xalu_OR, X86RMI_Reg(yLo), tLo));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* 32HLto64(e1,e2) */
   if (e->tag == Iex_Binop
       && e->Iex.Binop.op == Iop_32HLto64) {
      *rHi = iselIntExpr_R(env, e->Iex.Binop.arg1);
      *rLo = iselIntExpr_R(env, e->Iex.Binop.arg2);
      return;
   }

   /* 32Sto64(e) */
   if (e->tag == Iex_Unop
       && e->Iex.Unop.op == Iop_32Sto64) {
      HReg tLo = newVRegI(env);
      HReg tHi = newVRegI(env);
      HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
      addInstr(env, mk_MOVsd_RR(src,tHi));
      addInstr(env, mk_MOVsd_RR(src,tLo));
      addInstr(env, X86Instr_Sh32(Xsh_SAR, 31, X86RM_Reg(tHi)));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* could do better than this, but for now ... */
   if (e->tag == Iex_Unop
       && e->Iex.Unop.op == Iop_1Sto64) {
      HReg tLo = newVRegI(env);
      HReg tHi = newVRegI(env);
      X86CondCode cond = iselCondCode(env, e->Iex.Unop.arg);
      addInstr(env, X86Instr_Set32(cond,tLo));
      addInstr(env, X86Instr_Sh32(Xsh_SHL, 31, X86RM_Reg(tLo)));
      addInstr(env, X86Instr_Sh32(Xsh_SAR, 31, X86RM_Reg(tLo)));
      addInstr(env, mk_MOVsd_RR(tLo, tHi));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* 32Uto64(e) */
   if (e->tag == Iex_Unop
       && e->Iex.Unop.op == Iop_32Uto64) {
      HReg tLo = newVRegI(env);
      HReg tHi = newVRegI(env);
      HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
      addInstr(env, mk_MOVsd_RR(src,tLo));
      addInstr(env, X86Instr_Alu32R(Xalu_MOV, X86RMI_Imm(0), tHi));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* ReinterpF64asI64(e) */
   /* Given an IEEE754 double, produce an I64 with the same bit
      pattern. */
   if (e->tag == Iex_Unop
       && e->Iex.Unop.op == Iop_ReinterpF64asI64) {
      HReg rf   = iselDblExpr(env, e->Iex.Unop.arg);
      HReg tLo  = newVRegI(env);
      HReg tHi  = newVRegI(env);
      X86AMode* zero_esp = X86AMode_IR(0, hregX86_ESP());
      X86AMode* four_esp = X86AMode_IR(4, hregX86_ESP());
      /* subl $8, %esp */
      addInstr(env, 
               X86Instr_Alu32R(Xalu_SUB, X86RMI_Imm(8), hregX86_ESP()));
      /* gstD %rf, 0(%esp) */
      addInstr(env,
               X86Instr_FpLdSt(False/*store*/, 8, rf, zero_esp));
      /* movl 0(%esp), %tLo */
      addInstr(env, 
               X86Instr_Alu32R(Xalu_MOV, X86RMI_Mem(zero_esp), tLo));
      /* movl 4(%esp), %tHi */
      addInstr(env, 
               X86Instr_Alu32R(Xalu_MOV, X86RMI_Mem(four_esp), tHi));
      /* addl $8, %esp */
      addInstr(env, 
               X86Instr_Alu32R(Xalu_ADD, X86RMI_Imm(8), hregX86_ESP()));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* 64-bit shifts */
   if (e->tag == Iex_Binop
       && e->Iex.Binop.op == Iop_Shl64) {
      /* We use the same ingenious scheme as gcc.  Put the value
         to be shifted into %hi:%lo, and the shift amount into %cl.
         Then (dsts on right, a la ATT syntax):
 
            shldl %cl, %lo, %hi   -- make %hi be right for the shift amt
                                  -- %cl % 32
            shll  %cl, %lo        -- make %lo be right for the shift amt
                                  -- %cl % 32

         Now, if (shift amount % 64) is in the range 32 .. 63, we have 
         to do a fixup, which puts the result low half into the result
         high half, and zeroes the low half:

            testl $32, %ecx

            cmovnz %lo, %hi
            movl $0, %tmp         -- sigh; need yet another reg
            cmovnz %tmp, %lo
      */
      HReg rAmt, sHi, sLo, tHi, tLo, tTemp;
      tLo = newVRegI(env);
      tHi = newVRegI(env);
      tTemp = newVRegI(env);
      rAmt = iselIntExpr_R(env, e->Iex.Binop.arg2);
      iselIntExpr64(&sHi,&sLo, env, e->Iex.Binop.arg1);
      addInstr(env, mk_MOVsd_RR(rAmt, hregX86_ECX()));
      addInstr(env, mk_MOVsd_RR(sHi, tHi));
      addInstr(env, mk_MOVsd_RR(sLo, tLo));
      /* Ok.  Now shift amt is in %ecx, and value is in tHi/tLo and
         those regs are legitimately modifiable. */
      addInstr(env, X86Instr_Sh3232(Xsh_SHL, 0/*%cl*/, tLo, tHi));
      addInstr(env, X86Instr_Sh32(Xsh_SHL, 0/*%cl*/, X86RM_Reg(tLo)));
      addInstr(env, X86Instr_Test32(X86RI_Imm(32), X86RM_Reg(hregX86_ECX())));
      addInstr(env, X86Instr_CMov32(Xcc_NZ, X86RM_Reg(tLo), tHi));
      addInstr(env, X86Instr_Alu32R(Xalu_MOV, X86RMI_Imm(0), tTemp));
      addInstr(env, X86Instr_CMov32(Xcc_NZ, X86RM_Reg(tTemp), tLo));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   if (e->tag == Iex_Binop
       && e->Iex.Binop.op == Iop_Shr64) {
      /* We use the same ingenious scheme as gcc.  Put the value
         to be shifted into %hi:%lo, and the shift amount into %cl.
         Then:
 
            shrdl %cl, %hi, %lo   -- make %lo be right for the shift amt
                                  -- %cl % 32
            shrl  %cl, %hi        -- make %hi be right for the shift amt
                                  -- %cl % 32

         Now, if (shift amount % 64) is in the range 32 .. 63, we have 
         to do a fixup, which puts the result high half into the result
         low half, and zeroes the high half:

            testl $32, %ecx

            cmovnz %hi, %lo
            movl $0, %tmp         -- sigh; need yet another reg
            cmovnz %tmp, %hi
      */
      HReg rAmt, sHi, sLo, tHi, tLo, tTemp;
      tLo = newVRegI(env);
      tHi = newVRegI(env);
      tTemp = newVRegI(env);
      rAmt = iselIntExpr_R(env, e->Iex.Binop.arg2);
      iselIntExpr64(&sHi,&sLo, env, e->Iex.Binop.arg1);
      addInstr(env, mk_MOVsd_RR(rAmt, hregX86_ECX()));
      addInstr(env, mk_MOVsd_RR(sHi, tHi));
      addInstr(env, mk_MOVsd_RR(sLo, tLo));
      /* Ok.  Now shift amt is in %ecx, and value is in tHi/tLo and
         those regs are legitimately modifiable. */
      addInstr(env, X86Instr_Sh3232(Xsh_SHR, 0/*%cl*/, tHi, tLo));
      addInstr(env, X86Instr_Sh32(Xsh_SHR, 0/*%cl*/, X86RM_Reg(tHi)));
      addInstr(env, X86Instr_Test32(X86RI_Imm(32), X86RM_Reg(hregX86_ECX())));
      addInstr(env, X86Instr_CMov32(Xcc_NZ, X86RM_Reg(tHi), tLo));
      addInstr(env, X86Instr_Alu32R(Xalu_MOV, X86RMI_Imm(0), tTemp));
      addInstr(env, X86Instr_CMov32(Xcc_NZ, X86RM_Reg(tTemp), tHi));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* F64 -> I64 */
   /* Sigh, this is an almost exact copy of the F64 -> I32/I16 case.
      Unfortunately I see no easy way to avoid the duplication. */
   if (e->tag == Iex_Binop
       && e->Iex.Binop.op == Iop_F64toI64) {
      HReg rf   = iselDblExpr(env, e->Iex.Binop.arg2);
      HReg rrm  = iselIntExpr_R(env, e->Iex.Binop.arg1);
      HReg rrm2 = newVRegI(env);
      HReg tLo  = newVRegI(env);
      HReg tHi  = newVRegI(env);

      /* Used several times ... */
      /* Careful ... this sharing is only safe because
	 zero_esp/four_esp do not hold any registers which the
	 register allocator could attempt to swizzle later. */
      X86AMode* zero_esp = X86AMode_IR(0, hregX86_ESP());
      X86AMode* four_esp = X86AMode_IR(4, hregX86_ESP());

      /* rf now holds the value to be converted, and rrm holds the
         rounding mode value, encoded as per the IRRoundingMode enum.
         The first thing to do is set the FPU's rounding mode
         accordingly. */

      /* Create a space, both for the control word messing, and for
         the actual store conversion. */
      /* subl $8, %esp */
      addInstr(env, 
               X86Instr_Alu32R(Xalu_SUB, X86RMI_Imm(8), hregX86_ESP()));
      /* movl %rrm, %rrm2
         andl $3, %rrm2   -- shouldn't be needed; paranoia
         shll $10, %rrm2
         orl  $0x037F, %rrm2
         movl %rrm2, 0(%esp)
         fldcw 0(%esp)
      */
      addInstr(env, mk_MOVsd_RR(rrm, rrm2));
      addInstr(env, X86Instr_Alu32R(Xalu_AND, X86RMI_Imm(3), rrm2));
      addInstr(env, X86Instr_Sh32(Xsh_SHL, 10, X86RM_Reg(rrm2)));
      addInstr(env, X86Instr_Alu32R(Xalu_OR, X86RMI_Imm(0x037F), rrm2));
      addInstr(env, X86Instr_Alu32M(Xalu_MOV, X86RI_Reg(rrm2), zero_esp));
      addInstr(env, X86Instr_FpLdStCW(True/*load*/, zero_esp));

      /* gistll %rf, 0(%esp) */
      addInstr(env, X86Instr_FpLdStI(False/*store*/, 8, rf, zero_esp));

      /* movl 0(%esp), %dstLo */
      /* movl 4(%esp), %dstHi */
      addInstr(env, X86Instr_Alu32R(
                       Xalu_MOV, X86RMI_Mem(zero_esp), tLo));
      addInstr(env, X86Instr_Alu32R(
                       Xalu_MOV, X86RMI_Mem(four_esp), tHi));

      /* Restore default FPU control.
            movl $0x037F, 0(%esp)
            fldcw 0(%esp)
      */
      addInstr(env, X86Instr_Alu32M(Xalu_MOV, X86RI_Imm(0x037F), zero_esp));
      addInstr(env, X86Instr_FpLdStCW(True/*load*/, zero_esp));

      /* addl $8, %esp */
      addInstr(env, 
               X86Instr_Alu32R(Xalu_ADD, X86RMI_Imm(8), hregX86_ESP()));

      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* --------- CCALL --------- */
   if (e->tag == Iex_CCall) {
      HReg tLo = newVRegI(env);
      HReg tHi = newVRegI(env);

      /* Marshal args, do the call, clear stack. */
      doHelperCall( env, False, NULL, e->Iex.CCall.cee, e->Iex.CCall.args );

      addInstr(env, mk_MOVsd_RR(hregX86_EDX(), tHi));
      addInstr(env, mk_MOVsd_RR(hregX86_EAX(), tLo));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

#if 0
   if (e->tag == Iex_GetI) {
      /* First off, compute the index expression into an integer reg.
         The referenced address will then be 0 + ebp + reg*1, that is,
         an X86AMode_IRRS. */
      vassert(e->Iex.GetI.ty == Ity_I64);
      HReg tLo = newVRegI(env);
      HReg tHi = newVRegI(env);
      HReg idx = iselIntExpr_R(env, e->Iex.GetI.ixset);

      /* This (x86) is a little-endian target.  The front end will
	 have laid out the baseblock in accordance with the back-end's
	 endianness, so this hardwired assumption here that the 64-bit
	 value is stored little-endian is OK. */
      addInstr(env, X86Instr_Alu32R(
                       Xalu_MOV, 
                       X86RMI_Mem(X86AMode_IRRS(0, hregX86_EBP(), idx, 0)), 
                       tLo));
      addInstr(env, X86Instr_Alu32R(
                       Xalu_MOV, 
                       X86RMI_Mem(X86AMode_IRRS(4, hregX86_EBP(), idx, 0)), 
                       tHi));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }
#endif

   ppIRExpr(e);
   vpanic("iselIntExpr64");
}



/*---------------------------------------------------------*/
/*--- ISEL: Floating point expressions (64 bit)         ---*/
/*---------------------------------------------------------*/

/* Compute a 64-bit floating point value into a register, the identity
   of which is returned.  As with iselIntExpr_R, the reg may be either
   real or virtual; in any case it must not be changed by subsequent
   code emitted by the caller.  */

/* IEEE 754 formats.  From http://www.freesoft.org/CIE/RFC/1832/32.htm:

    Type                  S (1 bit)   E (11 bits)   F (52 bits)
    ----                  ---------   -----------   -----------
    signalling NaN        u           2047 (max)    .0uuuuu---u
                                                    (with at least
                                                     one 1 bit)
    quiet NaN             u           2047 (max)    .1uuuuu---u

    negative infinity     1           2047 (max)    .000000---0

    positive infinity     0           2047 (max)    .000000---0

    negative zero         1           0             .000000---0

    positive zero         0           0             .000000---0
*/

static HReg iselFltExpr ( ISelEnv* env, IRExpr* e )
{
   //   MatchInfo mi;
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_F32);

   if (e->tag == Iex_Tmp) {
      return lookupIRTemp(env, e->Iex.Tmp.tmp);
   }

   if (e->tag == Iex_LDle) {
      X86AMode* am;
      HReg res = newVRegF(env);
      vassert(e->Iex.LDle.ty == Ity_F32);
      am = iselIntExpr_AMode(env, e->Iex.LDle.addr);
      addInstr(env, X86Instr_FpLdSt(True/*load*/, 4, res, am));
      return res;
   }

   if (e->tag == Iex_Unop
       && e->Iex.Unop.op == Iop_F64toF32) {
      /* this is a no-op */
      return iselDblExpr(env, e->Iex.Unop.arg);
   }

   ppIRExpr(e);
   vpanic("iselFltExpr");
}


static HReg iselDblExpr ( ISelEnv* env, IRExpr* e )
{
   /* MatchInfo mi; */
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(e);
   vassert(ty == Ity_F64);

   if (e->tag == Iex_Tmp) {
      return lookupIRTemp(env, e->Iex.Tmp.tmp);
   }

   if (e->tag == Iex_Const) {
      union { UInt u32x2[2]; ULong u64; Double f64; } u;
      HReg freg = newVRegF(env);
      vassert(sizeof(u) == 8);
      vassert(sizeof(u.u64) == 8);
      vassert(sizeof(u.f64) == 8);
      vassert(sizeof(u.u32x2) == 8);

      if (e->Iex.Const.con->tag == Ico_F64) {
         u.f64 = e->Iex.Const.con->Ico.F64;
      }
      else if (e->Iex.Const.con->tag == Ico_F64i) {
         u.u64 = e->Iex.Const.con->Ico.F64i;
      }
      else
         vpanic("iselDblExpr(x86): const");

      addInstr(env, X86Instr_Push(X86RMI_Imm(u.u32x2[1])));
      addInstr(env, X86Instr_Push(X86RMI_Imm(u.u32x2[0])));
      addInstr(env, X86Instr_FpLdSt(True/*load*/, 8, freg, 
                                    X86AMode_IR(0, hregX86_ESP())));
      addInstr(env, X86Instr_Alu32R(Xalu_ADD,
                                    X86RMI_Imm(8),
                                    hregX86_ESP()));
      return freg;
   }

   if (e->tag == Iex_LDle) {
      X86AMode* am;
      HReg res = newVRegF(env);
      vassert(e->Iex.LDle.ty == Ity_F64);
      am = iselIntExpr_AMode(env, e->Iex.LDle.addr);
      addInstr(env, X86Instr_FpLdSt(True/*load*/, 8, res, am));
      return res;
   }

#if 0
   /* special-case: GetI( Add32(Shl32(y,3),const:I32) ):F64 */
   { DECLARE_PATTERN(p_Get_FP_reg);
     DEFINE_PATTERN(p_Get_FP_reg,
        IRExpr_GetI(
           binop(Iop_Add32, binop(Iop_Shl32,bind(0),mkU8(3)), bind(1)),
           Ity_F64,56,119
        )
     );
     if (matchIRExpr(&mi, p_Get_FP_reg, e)) {
        /* partial match, but we have to ensure bind(1) is a 32-bit
	   literal. */
        IRExpr* y = mi.bindee[0];
        IRExpr* c = mi.bindee[1];
        if (c->tag == Iex_Const && c->Iex.Const.con->tag == Ico_U32) {
           UInt c32 = c->Iex.Const.con->Ico.U32;
           HReg res = newVRegF(env);
           HReg ry = iselIntExpr_R(env, y);
           addInstr(env, 
                    X86Instr_FpLdSt( 
                       True/*load*/, 8, res,
                       X86AMode_IRRS(c32, hregX86_EBP(), ry, 3)) );
           return res;
        }
     }
   }
#endif

   /* GetI, default case */
   if (e->tag == Iex_GetI) {
      X86AMode* am 
         = genGuestArrayOffset(
              env, e->Iex.GetI.descr, 
                   e->Iex.GetI.ix, e->Iex.GetI.bias );
      HReg res = newVRegF(env);
      addInstr(env, X86Instr_FpLdSt( True/*load*/, 8, res, am ));
      return res;
   }

   if (e->tag == Iex_Binop) {
      X86FpOp fpop = Xfp_INVALID;
      switch (e->Iex.Binop.op) {
         case Iop_AddF64:    fpop = Xfp_ADD; break;
         case Iop_SubF64:    fpop = Xfp_SUB; break;
         case Iop_MulF64:    fpop = Xfp_MUL; break;
         case Iop_DivF64:    fpop = Xfp_DIV; break;
         case Iop_ScaleF64:  fpop = Xfp_SCALE; break;
         case Iop_AtanF64:   fpop = Xfp_ATAN; break;
         case Iop_Yl2xF64:   fpop = Xfp_YL2X; break;
         case Iop_Yl2xp1F64: fpop = Xfp_YL2XP1; break;
         case Iop_PRemF64:   fpop = Xfp_PREM; break;
         case Iop_PRem1F64:  fpop = Xfp_PREM1; break;
         default: break;
      }
      if (fpop != Xfp_INVALID) {
         HReg res  = newVRegF(env);
         HReg srcL = iselDblExpr(env, e->Iex.Binop.arg1);
         HReg srcR = iselDblExpr(env, e->Iex.Binop.arg2);
         addInstr(env, X86Instr_FpBinary(fpop,srcL,srcR,res));
         return res;
      }
   }

   if (e->tag == Iex_Binop && e->Iex.Binop.op == Iop_RoundF64) {
      HReg rf   = iselDblExpr(env, e->Iex.Binop.arg2);
      HReg rrm  = iselIntExpr_R(env, e->Iex.Binop.arg1);
      HReg rrm2 = newVRegI(env);
      HReg dst  = newVRegF(env);

      /* Used several times ... */
      /* Careful ... this sharing is only safe because
	 zero_esp does not hold any registers which the
	 register allocator could attempt to swizzle later. */
      X86AMode* zero_esp = X86AMode_IR(0, hregX86_ESP());

      /* rf now holds the value to be rounded, and rrm holds the
         rounding mode value, encoded as per the IRRoundingMode enum.
         The first thing to do is set the FPU's rounding mode
         accordingly. */

      /* subl $4, %esp */
      addInstr(env, 
               X86Instr_Alu32R(Xalu_SUB, X86RMI_Imm(4), hregX86_ESP()));
      /* movl %rrm, %rrm2
         andl $3, %rrm2   -- shouldn't be needed; paranoia
         shll $10, %rrm2
         orl  $0x037F, %rrm2
         movl %rrm2, 0(%esp)
         fldcw 0(%esp)
      */
      addInstr(env, mk_MOVsd_RR(rrm, rrm2));
      addInstr(env, X86Instr_Alu32R(Xalu_AND, X86RMI_Imm(3), rrm2));
      addInstr(env, X86Instr_Sh32(Xsh_SHL, 10, X86RM_Reg(rrm2)));
      addInstr(env, X86Instr_Alu32R(Xalu_OR, X86RMI_Imm(0x037F), rrm2));
      addInstr(env, X86Instr_Alu32M(Xalu_MOV, X86RI_Reg(rrm2), zero_esp));
      addInstr(env, X86Instr_FpLdStCW(True/*load*/, zero_esp));

      /* grndint %rf, %dst */
      addInstr(env, X86Instr_FpUnary(Xfp_ROUND, rf, dst));

      /* Restore default FPU control.
            movl $0x037F, 0(%esp)
            fldcw 0(%esp)
      */
      addInstr(env, X86Instr_Alu32M(Xalu_MOV, X86RI_Imm(0x037F), zero_esp));
      addInstr(env, X86Instr_FpLdStCW(True/*load*/, zero_esp));

      /* addl $4, %esp */
      addInstr(env, 
               X86Instr_Alu32R(Xalu_ADD, X86RMI_Imm(4), hregX86_ESP()));
      return dst;
   }


   if (e->tag == Iex_Unop) {
      X86FpOp fpop = Xfp_INVALID;
      switch (e->Iex.Unop.op) {
         case Iop_NegF64:  fpop = Xfp_NEG; break;
         case Iop_AbsF64:  fpop = Xfp_ABS; break;
         case Iop_SqrtF64: fpop = Xfp_SQRT; break;
         case Iop_SinF64:  fpop = Xfp_SIN; break;
         case Iop_CosF64:  fpop = Xfp_COS; break;
         case Iop_TanF64:  fpop = Xfp_TAN; break;
         case Iop_2xm1F64: fpop = Xfp_2XM1; break;
         default: break;
      }
      if (fpop != Xfp_INVALID) {
         HReg res = newVRegF(env);
         HReg src = iselDblExpr(env, e->Iex.Unop.arg);
         addInstr(env, X86Instr_FpUnary(fpop,src,res));
         return res;
      }
   }

   if (e->tag == Iex_Unop) {
      switch (e->Iex.Unop.op) {
         case Iop_I32toF64: {
            HReg dst = newVRegF(env);
            HReg ri  = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, X86Instr_Push(X86RMI_Reg(ri)));
            addInstr(env, X86Instr_FpLdStI(
                             True/*load*/, 4, dst, 
                             X86AMode_IR(0, hregX86_ESP())));
	    addInstr(env, X86Instr_Alu32R(Xalu_ADD,
                                          X86RMI_Imm(4),
                                          hregX86_ESP()));
            return dst;
         }
         case Iop_I64toF64: {
            HReg dst = newVRegF(env);
            HReg rHi,rLo;
	    iselIntExpr64( &rHi, &rLo, env, e->Iex.Unop.arg);
            addInstr(env, X86Instr_Push(X86RMI_Reg(rHi)));
            addInstr(env, X86Instr_Push(X86RMI_Reg(rLo)));
            addInstr(env, X86Instr_FpLdStI(
                             True/*load*/, 8, dst, 
                             X86AMode_IR(0, hregX86_ESP())));
	    addInstr(env, X86Instr_Alu32R(Xalu_ADD,
                                          X86RMI_Imm(8),
                                          hregX86_ESP()));
            return dst;
         }
         case Iop_ReinterpI64asF64: {
            /* Given an I64, produce an IEEE754 double with the same
               bit pattern. */
            HReg dst = newVRegF(env);
            HReg rHi, rLo;
	    iselIntExpr64( &rHi, &rLo, env, e->Iex.Unop.arg);
            addInstr(env, X86Instr_Push(X86RMI_Reg(rHi)));
            addInstr(env, X86Instr_Push(X86RMI_Reg(rLo)));
            addInstr(env, X86Instr_FpLdSt(
                             True/*load*/, 8, dst, 
                             X86AMode_IR(0, hregX86_ESP())));
	    addInstr(env, X86Instr_Alu32R(Xalu_ADD,
                                          X86RMI_Imm(8),
                                          hregX86_ESP()));
            return dst;
	 }
         case Iop_F32toF64:
            /* this is a no-op */
            return iselFltExpr(env, e->Iex.Unop.arg);
         default: 
            break;
      }
   }

   /* --------- MULTIPLEX --------- */
   if (e->tag == Iex_Mux0X) {
     if (ty == Ity_F64
         && typeOfIRExpr(env->type_env,e->Iex.Mux0X.cond) == Ity_I8) {
        HReg r8  = iselIntExpr_R(env, e->Iex.Mux0X.cond);
        HReg rX  = iselDblExpr(env, e->Iex.Mux0X.exprX);
        HReg r0  = iselDblExpr(env, e->Iex.Mux0X.expr0);
        HReg dst = newVRegF(env);
        addInstr(env, X86Instr_FpUnary(Xfp_MOV,rX,dst));
        addInstr(env, X86Instr_Test32(X86RI_Imm(0xFF), X86RM_Reg(r8)));
        addInstr(env, X86Instr_FpCMov(Xcc_Z,r0,dst));
        return dst;
      }
   }

   ppIRExpr(e);
   vpanic("iselDblExpr");
}



/*---------------------------------------------------------*/
/*--- ISEL: Statements                                  ---*/
/*---------------------------------------------------------*/

static void iselStmt ( ISelEnv* env, IRStmt* stmt )
{
   if (vex_traceflags & VEX_TRACE_VCODE) {
      vex_printf("\n-- ");
      ppIRStmt(stmt);
      vex_printf("\n");
   }

   switch (stmt->tag) {

   /* --------- STORE --------- */
   case Ist_STle: {
      X86AMode* am;
      IRType tya = typeOfIRExpr(env->type_env, stmt->Ist.STle.addr);
      IRType tyd = typeOfIRExpr(env->type_env, stmt->Ist.STle.data);
      vassert(tya == Ity_I32);
      am = iselIntExpr_AMode(env, stmt->Ist.STle.addr);
      if (tyd == Ity_I32) {
         X86RI* ri = iselIntExpr_RI(env, stmt->Ist.STle.data);
         addInstr(env, X86Instr_Alu32M(Xalu_MOV,ri,am));
         return;
      }
      if (tyd == Ity_I8 || tyd == Ity_I16) {
         HReg r = iselIntExpr_R(env, stmt->Ist.STle.data);
         addInstr(env, X86Instr_Store(tyd==Ity_I8 ? 1 : 2,
                                      r,am));
         return;
      }
      if (tyd == Ity_F64) {
         HReg r = iselDblExpr(env, stmt->Ist.STle.data);
         addInstr(env, X86Instr_FpLdSt(False/*store*/, 8, r, am));
         return;
      }
      if (tyd == Ity_F32) {
         HReg r = iselFltExpr(env, stmt->Ist.STle.data);
         addInstr(env, X86Instr_FpLdSt(False/*store*/, 4, r, am));
         return;
      }
      if (tyd == Ity_I64) {
         HReg vHi, vLo, rA;
         iselIntExpr64(&vHi, &vLo, env, stmt->Ist.STle.data);
         rA = iselIntExpr_R(env, stmt->Ist.STle.addr);
         addInstr(env, X86Instr_Alu32M(
                          Xalu_MOV, X86RI_Reg(vLo), X86AMode_IR(0, rA)));
         addInstr(env, X86Instr_Alu32M(
                          Xalu_MOV, X86RI_Reg(vHi), X86AMode_IR(4, rA)));
         return;
      }
      break;
   }

   /* --------- PUT --------- */
   case Ist_Put: {
      IRType ty = typeOfIRExpr(env->type_env, stmt->Ist.Put.data);
      if (ty == Ity_I32) {
         /* We're going to write to memory, so compute the RHS into an
            X86RI. */
         X86RI* ri  = iselIntExpr_RI(env, stmt->Ist.Put.data);
         addInstr(env,
                  X86Instr_Alu32M(
                     Xalu_MOV,
                     ri,
                     X86AMode_IR(stmt->Ist.Put.offset,hregX86_EBP())
                 ));
         return;
      }
      if (ty == Ity_I8 || ty == Ity_I16) {
         HReg r = iselIntExpr_R(env, stmt->Ist.Put.data);
         addInstr(env, X86Instr_Store(
                          ty==Ity_I8 ? 1 : 2,
                          r,
                          X86AMode_IR(stmt->Ist.Put.offset,
                                      hregX86_EBP())));
         return;
      }
      break;
   }

   /* --------- Indexed PUT --------- */
   case Ist_PutI: {
      X86AMode* am 
         = genGuestArrayOffset(
              env, stmt->Ist.PutI.descr, 
                   stmt->Ist.PutI.ix, stmt->Ist.PutI.bias );

      IRType ty = typeOfIRExpr(env->type_env, stmt->Ist.PutI.data);
      if (ty == Ity_F64) {
         HReg val = iselDblExpr(env, stmt->Ist.PutI.data);
         addInstr(env, X86Instr_FpLdSt( False/*store*/, 8, val, am ));
         return;
      }
      if (ty == Ity_I8) {
         HReg r = iselIntExpr_R(env, stmt->Ist.PutI.data);
         addInstr(env, X86Instr_Store( 1, r, am ));
         return;
      }
      if (ty == Ity_I64) {
         HReg rHi, rLo;
         X86AMode* am4 = advance4(am);
         iselIntExpr64(&rHi, &rLo, env, stmt->Ist.PutI.data);
         addInstr(env, X86Instr_Alu32M( Xalu_MOV, X86RI_Reg(rLo), am ));
         addInstr(env, X86Instr_Alu32M( Xalu_MOV, X86RI_Reg(rHi), am4 ));
         return;
      }
      break;
   }

   /* --------- TMP --------- */
   case Ist_Tmp: {
      IRTemp tmp = stmt->Ist.Tmp.tmp;
      IRType ty = typeOfIRTemp(env->type_env, tmp);
      if (ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8) {
         X86RMI* rmi = iselIntExpr_RMI(env, stmt->Ist.Tmp.data);
         HReg dst = lookupIRTemp(env, tmp);
         addInstr(env, X86Instr_Alu32R(Xalu_MOV,rmi,dst));
         return;
      }
      if (ty == Ity_I64) {
         HReg rHi, rLo, dstHi, dstLo;
         iselIntExpr64(&rHi,&rLo, env, stmt->Ist.Tmp.data);
         lookupIRTemp64( &dstHi, &dstLo, env, tmp);
         addInstr(env, mk_MOVsd_RR(rHi,dstHi) );
         addInstr(env, mk_MOVsd_RR(rLo,dstLo) );
         return;
      }
      if (ty == Ity_Bit) {
         X86CondCode cond = iselCondCode(env, stmt->Ist.Tmp.data);
         HReg dst = lookupIRTemp(env, tmp);
         addInstr(env, X86Instr_Set32(cond, dst));
         return;
      }
      if (ty == Ity_F64) {
         HReg dst = lookupIRTemp(env, tmp);
         HReg src = iselDblExpr(env, stmt->Ist.Tmp.data);
         addInstr(env, X86Instr_FpUnary(Xfp_MOV,src,dst));
         return;
      }
      if (ty == Ity_F32) {
         HReg dst = lookupIRTemp(env, tmp);
         HReg src = iselFltExpr(env, stmt->Ist.Tmp.data);
         addInstr(env, X86Instr_FpUnary(Xfp_MOV,src,dst));
         return;
      }
      break;
   }

   /* --------- Call to DIRTY helper --------- */
   case Ist_Dirty: {
      IRType   retty;
      IRDirty* d = stmt->Ist.Dirty.details;
      Bool     passBBP = False;

      if (d->nFxState == 0)
         vassert(!d->needsBBP);
      passBBP = d->nFxState > 0 && d->needsBBP;

      /* Marshal args, do the call, clear stack. */
      doHelperCall( env, passBBP, d->guard, d->cee, d->args );

      /* Now figure out what to do with the returned value, if any. */
      if (d->tmp == INVALID_IRTEMP)
         /* No return value.  Nothing to do. */
         return;

      retty = typeOfIRTemp(env->type_env, d->tmp);
      if (retty == Ity_I64) {
         HReg dstHi, dstLo;
         /* The returned value is in %edx:%eax.  Park it in the
            register-pair associated with tmp. */
         lookupIRTemp64( &dstHi, &dstLo, env, d->tmp);
         addInstr(env, mk_MOVsd_RR(hregX86_EDX(),dstHi) );
         addInstr(env, mk_MOVsd_RR(hregX86_EAX(),dstLo) );
         return;
      }
      if (retty == Ity_I32 || retty == Ity_I16 || retty == Ity_I8) {
         /* The returned value is in %eax.  Park it in the register
            associated with tmp. */
         HReg dst = lookupIRTemp(env, d->tmp);
         addInstr(env, mk_MOVsd_RR(hregX86_EAX(),dst) );
         return;
      }
      break;
   }

   /* --------- EXIT --------- */
   case Ist_Exit: {
      X86RI*      dst;
      X86CondCode cc;
      if (stmt->Ist.Exit.dst->tag != Ico_U32)
         vpanic("isel_x86: Ist_Exit: dst is not a 32-bit value");
      dst = iselIntExpr_RI(env, IRExpr_Const(stmt->Ist.Exit.dst));
      cc  = iselCondCode(env,stmt->Ist.Exit.cond);
      addInstr(env, X86Instr_Goto(Ijk_Boring, cc, dst));
      return;
   }

   default: break;
   }
   ppIRStmt(stmt);
   vpanic("iselStmt");
}


/*---------------------------------------------------------*/
/*--- ISEL: Basic block terminators (Nexts)             ---*/
/*---------------------------------------------------------*/

static void iselNext ( ISelEnv* env, IRExpr* next, IRJumpKind jk )
{
   X86RI* ri;
   if (vex_traceflags & VEX_TRACE_VCODE) {
      vex_printf("\n-- goto {");
      ppIRJumpKind(jk);
      vex_printf("} ");
      ppIRExpr(next);
      vex_printf("\n");
   }
   ri = iselIntExpr_RI(env, next);
   addInstr(env, X86Instr_Goto(jk, Xcc_ALWAYS,ri));
}


/*---------------------------------------------------------*/
/*--- Insn selector top-level                           ---*/
/*---------------------------------------------------------*/

/* Translate an entire BB to x86 code. */

HInstrArray* iselBB_X86 ( IRBB* bb )
{
   Int     i, j;
   HReg    hreg, hregHI;

   /* Make up an initial environment to use. */
   ISelEnv* env = LibVEX_Alloc(sizeof(ISelEnv));
   env->vreg_ctr = 0;

   /* Set up output code array. */
   env->code = newHInstrArray();

   /* Copy BB's type env. */
   env->type_env = bb->tyenv;

   /* Make up an IRTemp -> virtual HReg mapping.  This doesn't
      change as we go along. */
   env->n_vregmap = bb->tyenv->types_used;
   env->vregmap   = LibVEX_Alloc(env->n_vregmap * sizeof(HReg));
   env->vregmapHI = LibVEX_Alloc(env->n_vregmap * sizeof(HReg));

   /* For each IR temporary, allocate a suitably-kinded virtual
      register. */
   j = 0;
   for (i = 0; i < env->n_vregmap; i++) {
      hregHI = hreg = INVALID_HREG;
      switch (bb->tyenv->types[i]) {
         case Ity_Bit:
         case Ity_I8:
         case Ity_I16:
         case Ity_I32: hreg   = mkHReg(j++, HRcInt, True); break;
         case Ity_I64: hreg   = mkHReg(j++, HRcInt, True);
                       hregHI = mkHReg(j++, HRcInt, True); break;
         case Ity_F32:
         case Ity_F64: hreg   = mkHReg(j++, HRcFloat, True); break;
         default: ppIRType(bb->tyenv->types[i]);
                  vpanic("iselBB: IRTemp type");
      }
      env->vregmap[i]   = hreg;
      env->vregmapHI[i] = hregHI;
   }
   env->vreg_ctr = j;

   /* Ok, finally we can iterate over the statements. */
   for (i = 0; i < bb->stmts_used; i++)
      if (bb->stmts[i])
         iselStmt(env,bb->stmts[i]);

   iselNext(env,bb->next,bb->jumpkind);

   /* record the number of vregs we used. */
   env->code->n_vregs = env->vreg_ctr;
   return env->code;
}


/*---------------------------------------------------------------*/
/*--- end                                     host-x86/isel.c ---*/
/*---------------------------------------------------------------*/
