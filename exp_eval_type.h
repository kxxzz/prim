#pragma once



#include "exp.h"



typedef enum EXP_EvalTypeType
{
    EXP_EvalTypeType_Nval,
    EXP_EvalTypeType_Var,
    EXP_EvalTypeType_Fun,
    EXP_EvalTypeType_Tuple,
    EXP_EvalTypeType_Array,

    EXP_NumEvalTypeTypes
} EXP_EvalTypeType;

typedef struct EXP_EvalTypeDescList
{
    u32 count;
    u32 listId;
} EXP_EvalTypeDescList;

typedef struct EXP_EvalTypeDescFun
{
    EXP_EvalTypeDescList ins;
    EXP_EvalTypeDescList outs;
} EXP_EvalTypeDescFun;

typedef struct EXP_EvalTypeDesc
{
    EXP_EvalTypeType type;
    union
    {
        u32 nvalTypeId;
        EXP_EvalTypeDescFun fun;
        EXP_EvalTypeDescList tuple;
        u32 aryElm;
    };
} EXP_EvalTypeDesc;




typedef struct EXP_EvalTypeContext EXP_EvalTypeContext;

EXP_EvalTypeContext* EXP_newEvalTypeContext(void);
void EXP_evalTypeContextFree(EXP_EvalTypeContext* ctx);



u32 EXP_evalTypeNval(EXP_EvalTypeContext* ctx, u32 nativeType);
u32 EXP_evalTypeVar(EXP_EvalTypeContext* ctx, u32 varId);
u32 EXP_evalTypeFun(EXP_EvalTypeContext* ctx, u32 numIns, const u32* ins, u32 numOuts, const u32* outs);
u32 EXP_evalTypeTuple(EXP_EvalTypeContext* ctx, u32 count, const u32* elms);
u32 EXP_evalTypeArray(EXP_EvalTypeContext* ctx, u32 elm);



const EXP_EvalTypeDesc* EXP_evalTypeDescById(EXP_EvalTypeContext* ctx, u32 typeId);
const u32* EXP_evalTypeListById(EXP_EvalTypeContext* ctx, u32 listId);




































































































