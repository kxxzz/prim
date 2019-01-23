#include "exp_eval_a.h"





static EXP_EvalContext EXP_newEvalContext
(
    EXP_Space* space, EXP_EvalDataStack* dataStack, const EXP_EvalNativeEnv* nativeEnv,
    EXP_NodeSrcInfoTable* srcInfoTable
)
{
    EXP_EvalContext _ctx = { 0 };
    EXP_EvalContext* ctx = &_ctx;
    ctx->space = space;
    ctx->dataStack = dataStack;
    ctx->srcInfoTable = srcInfoTable;
    for (u32 i = 0; i < EXP_NumEvalPrimValueTypes; ++i)
    {
        vec_push(&ctx->valueTypeTable, EXP_EvalPrimValueTypeInfoTable[i]);
    }
    for (u32 i = 0; i < EXP_NumEvalPrimFuns; ++i)
    {
        vec_push(&ctx->nativeFunTable, EXP_EvalPrimFunInfoTable[i]);
    }
    if (nativeEnv)
    {
        for (u32 i = 0; i < nativeEnv->numValueTypes; ++i)
        {
            vec_push(&ctx->valueTypeTable, nativeEnv->valueTypes[i]);
        }
        for (u32 i = 0; i < nativeEnv->numFuns; ++i)
        {
            vec_push(&ctx->nativeFunTable, nativeEnv->funs[i]);
        }
    }
    return *ctx;
}
static void EXP_evalContextFree(EXP_EvalContext* ctx)
{
    vec_free(&ctx->blockStack);
    vec_free(&ctx->defStack);
    vec_free(&ctx->nativeFunTable);
    vec_free(&ctx->valueTypeTable);
}




static u32 EXP_evalGetNativeFun(EXP_EvalContext* ctx, const char* funName)
{
    EXP_Space* space = ctx->space;
    for (u32 i = 0; i < ctx->nativeFunTable.length; ++i)
    {
        u32 idx = ctx->nativeFunTable.length - 1 - i;
        const char* name = ctx->nativeFunTable.data[idx].name;
        if (0 == strcmp(funName, name))
        {
            return idx;
        }
    }
    return -1;
}










static bool EXP_evalCheckCall(EXP_Space* space, EXP_Node node)
{
    if (!EXP_isSeq(space, node))
    {
        return false;
    }
    u32 len = EXP_seqLen(space, node);
    if (!len)
    {
        return false;
    }
    EXP_Node* elms = EXP_seqElm(space, node);
    if (!EXP_isTok(space, elms[0]))
    {
        return false;
    }
    return true;
}

static bool EXP_evalCheckDefPat(EXP_Space* space, EXP_Node node)
{
    if (!EXP_isSeq(space, node))
    {
        return false;
    }
    u32 len = EXP_seqLen(space, node);
    if (!len)
    {
        return false;
    }
    EXP_Node* elms = EXP_seqElm(space, node);
    if (!EXP_isTok(space, elms[0]))
    {
        return false;
    }
    return true;
}




static void EXP_evalErrorAtNode(EXP_EvalContext* ctx, EXP_Node node, EXP_EvalErrCode errCode)
{
    ctx->ret.errCode = errCode;
    EXP_NodeSrcInfoTable* srcInfoTable = ctx->srcInfoTable;
    if (srcInfoTable)
    {
        assert(node.id < srcInfoTable->length);
        ctx->ret.errSrcFile = NULL;// todo
        ctx->ret.errSrcFileLine = srcInfoTable->data[node.id].line;
        ctx->ret.errSrcFileColumn = srcInfoTable->data[node.id].column;
    }
}





static void EXP_evalLoadDef(EXP_EvalContext* ctx, EXP_Node node)
{
    EXP_Space* space = ctx->space;
    if (EXP_isTok(space, node))
    {
        return;
    }
    if (!EXP_evalCheckCall(space, node))
    {
        EXP_evalErrorAtNode(ctx, node, EXP_EvalErrCode_EvalSyntax);
        return;
    }
    EXP_Node* defCall = EXP_seqElm(space, node);
    const char* kDef = EXP_tokCstr(space, defCall[0]);
    u32 nativeFun = EXP_evalGetNativeFun(ctx, kDef);
    if (nativeFun != EXP_EvalPrimFun_Def)
    {
        return;
    }
    EXP_Node name;
    if (EXP_isTok(space, defCall[1]))
    {
        name = defCall[1];
    }
    else if (EXP_evalCheckDefPat(space, defCall[1]))
    {
        EXP_Node* pat = EXP_seqElm(space, defCall[1]);
        name = pat[0];
    }
    else
    {
        EXP_evalErrorAtNode(ctx, defCall[1], EXP_EvalErrCode_EvalSyntax);
        return;
    }
    EXP_EvalDef def = { name, false, .fun = node };
    vec_push(&ctx->defStack, def);
}




static void EXP_evalDefGetParms(EXP_EvalContext* ctx, EXP_Node node, u32* pNumParms, EXP_Node** pParms)
{
    EXP_Space* space = ctx->space;
    EXP_Node* defCall = EXP_seqElm(space, node);
    if (EXP_isTok(space, defCall[1]))
    {
        *pNumParms = 0;
        *pParms = NULL;
    }
    else
    {
        assert(EXP_evalCheckDefPat(space, defCall[1]));
        EXP_Node* pat = EXP_seqElm(space, defCall[1]);
        *pNumParms = EXP_seqLen(space, defCall[1]) - 1;
        *pParms = pat + 1;
    }
}


static void EXP_evalDefGetBody(EXP_EvalContext* ctx, EXP_Node node, u32* pLen, EXP_Node** pSeq)
{
    EXP_Space* space = ctx->space;
    assert(EXP_seqLen(space, node) >= 2);
    *pLen = EXP_seqLen(space, node) - 2;
    EXP_Node* defCall = EXP_seqElm(space, node);
    *pSeq = defCall + 2;
}











static EXP_EvalDef* EXP_evalGetMatched(EXP_EvalContext* ctx, const char* funName)
{
    EXP_Space* space = ctx->space;
    for (u32 i = 0; i < ctx->defStack.length; ++i)
    {
        EXP_EvalDef* def = ctx->defStack.data + ctx->defStack.length - 1 - i;
        const char* str = EXP_tokCstr(space, def->key);
        if (0 == strcmp(str, funName))
        {
            return def;
        }
    }
    return NULL;
}














static bool EXP_evalEnterBlock
(
    EXP_EvalContext* ctx, u32 len, EXP_Node* seq, u32 numParms, EXP_Node* parms, EXP_Node srcNode
)
{
    static EXP_EvalBlockCallback nocb = { EXP_EvalBlockCallbackType_NONE };

    u32 argsOffset = ctx->dataStack->length - numParms;
    u32 defStackP = ctx->defStack.length;

    for (u32 i = 0; i < numParms; ++i)
    {
        EXP_Node k = parms[i];
        const char* ks = EXP_tokCstr(ctx->space, k);
        EXP_EvalValue v = ctx->dataStack->data[argsOffset + i];
        EXP_EvalDef def = { k, true,.val = v };
        vec_push(&ctx->defStack, def);
    }

    vec_resize(ctx->dataStack, argsOffset);
    u32 dataStackP = ctx->dataStack->length;

    for (u32 i = 0; i < len; ++i)
    {
        EXP_evalLoadDef(ctx, seq[i]);
        if (ctx->ret.errCode)
        {
            return false;;
        }
    }
    EXP_EvalBlock blk = { srcNode, defStackP, dataStackP, seq, len, 0, nocb };
    vec_push(&ctx->blockStack, blk);
    return true;
}

static bool EXP_evalEnterBlockWithCB
(
    EXP_EvalContext* ctx, u32 len, EXP_Node* seq, EXP_Node srcNode, EXP_EvalBlockCallback cb
)
{
    u32 defStackP = ctx->defStack.length;
    u32 dataStackP = ctx->dataStack->length;
    assert(cb.type != EXP_EvalBlockCallbackType_NONE);
    EXP_EvalBlock blk = { srcNode, defStackP, dataStackP, seq, len, 0, cb };
    vec_push(&ctx->blockStack, blk);
    return true;
}

static bool EXP_evalLeaveBlock(EXP_EvalContext* ctx)
{
    u32 defStackP = vec_last(&ctx->blockStack).defStackP;
    vec_resize(&ctx->defStack, defStackP);
    vec_pop(&ctx->blockStack);
    return ctx->blockStack.length > 0;
}




static bool EXP_evalValueTypeConvert(EXP_EvalContext* ctx, EXP_EvalValue* v, u32 vt, EXP_Node srcNode)
{
    EXP_Space* space = ctx->space;
    if (v->type != vt)
    {
        EXP_EvalValFromStr fromStr = ctx->valueTypeTable.data[vt].fromStr;
        if (fromStr && (EXP_EvalPrimValueType_Tok == v->type))
        {
            u32 l = EXP_tokSize(space, v->data.node);
            const char* s = EXP_tokCstr(space, v->data.node);
            EXP_EvalValueData data;
            if (!fromStr(l, s, &data))
            {
                EXP_evalErrorAtNode(ctx, srcNode, EXP_EvalErrCode_EvalArgs);
                return false;
            }
            v->type = vt;
            v->data = data;
        }
        else
        {
            EXP_evalErrorAtNode(ctx, srcNode, EXP_EvalErrCode_EvalArgs);
            return false;
        }
    }
    return true;
}



static void EXP_evalNativeFunCall
(
    EXP_EvalContext* ctx, EXP_EvalNativeFunInfo* nativeFunInfo, u32 argsOffset, EXP_Node srcNode
)
{
    EXP_Space* space = ctx->space;
    for (u32 i = 0; i < nativeFunInfo->numIns; ++i)
    {
        EXP_EvalValue* v = ctx->dataStack->data + argsOffset + i;
        u32 vt = nativeFunInfo->inType[i];
        if (!EXP_evalValueTypeConvert(ctx, v, vt, srcNode))
        {
            return;
        }
    }
    nativeFunInfo->call(space, ctx->dataStack->data + argsOffset, ctx->nativeCallOutBuf);
    vec_resize(ctx->dataStack, argsOffset);
    for (u32 i = 0; i < nativeFunInfo->numOuts; ++i)
    {
        u32 t = nativeFunInfo->outType[i];
        EXP_EvalValueData d = ctx->nativeCallOutBuf[i];
        EXP_EvalValue v = { t, d };
        vec_push(ctx->dataStack, v);
    }
}



static void EXP_evalCall(EXP_EvalContext* ctx)
{
    EXP_Space* space = ctx->space;
    EXP_EvalBlock* curBlock;
next:
    if (ctx->ret.errCode)
    {
        return;
    }
    curBlock = &vec_last(&ctx->blockStack);
    if (curBlock->p == curBlock->seqLen)
    {
        EXP_EvalBlockCallback* cb = &curBlock->cb;
        switch (cb->type)
        {
        case EXP_EvalBlockCallbackType_NONE:
        {
            break;
        }
        case EXP_EvalBlockCallbackType_NativeFun:
        {
            if (curBlock->dataStackP > ctx->dataStack->length)
            {
                EXP_evalErrorAtNode(ctx, curBlock->srcNode, EXP_EvalErrCode_EvalArgs);
                return;
            }
            u32 numIns = ctx->dataStack->length - curBlock->dataStackP;
            EXP_EvalNativeFunInfo* nativeFunInfo = ctx->nativeFunTable.data + cb->nativeFun;
            if (numIns != nativeFunInfo->numIns)
            {
                EXP_evalErrorAtNode(ctx, curBlock->srcNode, EXP_EvalErrCode_EvalArgs);
                return;
            }
            EXP_evalNativeFunCall(ctx, nativeFunInfo, curBlock->dataStackP, curBlock->srcNode);
            break;
        }
        case EXP_EvalBlockCallbackType_Fun:
        {
            if (curBlock->dataStackP > ctx->dataStack->length)
            {
                EXP_evalErrorAtNode(ctx, curBlock->srcNode, EXP_EvalErrCode_EvalArgs);
                return;
            }
            EXP_Node fun = cb->fun;
            u32 numParms = 0;
            EXP_Node* parms = NULL;
            EXP_evalDefGetParms(ctx, fun, &numParms, &parms);
            // todo
            if (curBlock->dataStackP > (ctx->dataStack->length - numParms))
            {
                EXP_evalErrorAtNode(ctx, curBlock->srcNode, EXP_EvalErrCode_EvalArgs);
                return;
            }
            u32 bodyLen = 0;
            EXP_Node* body = NULL;
            EXP_evalDefGetBody(ctx, fun, &bodyLen, &body);
            if (!EXP_evalLeaveBlock(ctx))
            {
                return;
            }
            if (EXP_evalEnterBlock(ctx, bodyLen, body, numParms, parms, curBlock->srcNode))
            {
                goto next;
            }
            return;
        }
        case EXP_EvalBlockCallbackType_Branch:
        {
            if (curBlock->dataStackP + 1 != ctx->dataStack->length)
            {
                EXP_evalErrorAtNode(ctx, curBlock->srcNode, EXP_EvalErrCode_EvalArgs);
                return;
            }
            EXP_EvalValue v = ctx->dataStack->data[curBlock->dataStackP];
            vec_pop(ctx->dataStack);
            if (!EXP_evalValueTypeConvert(ctx, &v, EXP_EvalPrimValueType_Bool, curBlock->srcNode))
            {
                return;
            }
            if (!EXP_evalLeaveBlock(ctx))
            {
                return;
            }
            if (v.data.b)
            {
                if (EXP_evalEnterBlock(ctx, 1, cb->branch[0], 0, NULL, curBlock->srcNode))
                {
                    goto next;
                }
            }
            else if (cb->branch[1])
            {
                if (EXP_evalEnterBlock(ctx, 1, cb->branch[1], 0, NULL, curBlock->srcNode))
                {
                    goto next;
                }
            }
            goto next;
        }
        default:
            assert(false);
            return;
        }
        if (EXP_evalLeaveBlock(ctx))
        {
            goto next;
        }
        return;
    }
    EXP_Node node = curBlock->seq[curBlock->p++];
    if (EXP_isTok(space, node))
    {
        const char* funName = EXP_tokCstr(space, node);
        u32 nativeFun = EXP_evalGetNativeFun(ctx, funName);
        if (nativeFun != -1)
        {
            EXP_EvalNativeFunInfo* nativeFunInfo = ctx->nativeFunTable.data + nativeFun;
            if (!nativeFunInfo->call)
            {
                EXP_evalErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                return;
            }
            if (ctx->dataStack->length < nativeFunInfo->numIns)
            {
                EXP_evalErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                return;
            }
            u32 argsOffset = ctx->dataStack->length - nativeFunInfo->numIns;
            EXP_evalNativeFunCall(ctx, nativeFunInfo, argsOffset, node);
            goto next;
        }
        EXP_EvalDef* def = EXP_evalGetMatched(ctx, funName);
        if (def)
        {
            if (def->isVal)
            {
                vec_push(ctx->dataStack, def->val);
                goto next;
            }
            else
            {
                u32 numParms = 0;;
                EXP_Node* parms = NULL;
                EXP_evalDefGetParms(ctx, def->fun, &numParms, &parms);
                if (ctx->dataStack->length < numParms)
                {
                    EXP_evalErrorAtNode(ctx, curBlock->srcNode, EXP_EvalErrCode_EvalArgs);
                    return;
                }
                u32 bodyLen = 0;
                EXP_Node* body = NULL;
                EXP_evalDefGetBody(ctx, def->fun, &bodyLen, &body);
                if (EXP_evalEnterBlock(ctx, bodyLen, body, numParms, parms, node))
                {
                    goto next;
                }
                else
                {
                    return;
                }
            }
        }
        else
        {
            EXP_EvalValue v = { EXP_EvalPrimValueType_Tok, .data.node = node };
            vec_push(ctx->dataStack, v);
            goto next;
        }
    }
    else if (!EXP_evalCheckCall(space, node))
    {
        EXP_evalErrorAtNode(ctx, node, EXP_EvalErrCode_EvalSyntax);
        return;
    }
    EXP_Node call = node;
    EXP_Node* elms = EXP_seqElm(space, call);
    u32 len = EXP_seqLen(space, call);
    const char* funName = EXP_tokCstr(space, elms[0]);
    EXP_EvalDef* def = EXP_evalGetMatched(ctx, funName);
    if (def)
    {
        if (def->isVal)
        {
            vec_push(ctx->dataStack, def->val);
            goto next;
        }
        else
        {
            u32 numParms = 0;;
            EXP_Node* parms = NULL;
            EXP_evalDefGetParms(ctx, def->fun, &numParms, &parms);
            EXP_EvalBlockCallback cb = { EXP_EvalBlockCallbackType_Fun, .fun = def->fun };
            if (EXP_evalEnterBlockWithCB(ctx, len - 1, elms + 1, node, cb))
            {
                goto next;
            }
            else
            {
                return;
            }
        }
    }
    u32 nativeFun = EXP_evalGetNativeFun(ctx, funName);
    switch (nativeFun)
    {
    case EXP_EvalPrimFun_Def:
    {
        goto next;
    }
    case EXP_EvalPrimFun_Blk:
    {
        // todo
        goto next;
    }
    case EXP_EvalPrimFun_If:
    {
        if ((len != 3) && (len != 4))
        {
            EXP_evalErrorAtNode(ctx, curBlock->srcNode, EXP_EvalErrCode_EvalArgs);
            return;
        }
        EXP_EvalBlockCallback cb = { EXP_EvalBlockCallbackType_Branch };
        cb.branch[0] = elms + 2;
        if (3 == len)
        {
            cb.branch[1] = NULL;
        }
        else if (4 == len)
        {
            cb.branch[1] = elms + 3;
        }
        if (EXP_evalEnterBlockWithCB(ctx, 1, elms + 1, node, cb))
        {
            goto next;
        }
        else
        {
            return;
        }
    }
    default:
    {
        if (nativeFun != -1)
        {
            EXP_EvalNativeFunInfo* nativeFunInfo = ctx->nativeFunTable.data + nativeFun;
            assert(nativeFunInfo->call);
            EXP_EvalBlockCallback cb = { EXP_EvalBlockCallbackType_NativeFun, .nativeFun = nativeFun };
            if (EXP_evalEnterBlockWithCB(ctx, len - 1, elms + 1, node, cb))
            {
                goto next;
            }
            else
            {
                return;
            }
        }
        EXP_evalErrorAtNode(ctx, call, EXP_EvalErrCode_EvalSyntax);
        break;
    }
    }
}
















EXP_EvalRet EXP_eval
(
    EXP_Space* space, EXP_EvalDataStack* dataStack, EXP_Node root, const EXP_EvalNativeEnv* nativeEnv,
    EXP_NodeSrcInfoTable* srcInfoTable
)
{
    EXP_EvalRet ret = { 0 };
    if (!EXP_isSeq(space, root))
    {
        return ret;
    }
    EXP_EvalContext ctx = EXP_newEvalContext(space, dataStack, nativeEnv, srcInfoTable);
    u32 len = EXP_seqLen(space, root);
    EXP_Node* seq = EXP_seqElm(space, root);
    EXP_evalEnterBlock(&ctx, len, seq, 0, NULL, root);
    EXP_evalCall(&ctx);
    ret = ctx.ret;
    EXP_evalContextFree(&ctx);
    return ret;
}












EXP_EvalRet EXP_evalFile
(
    EXP_Space* space, EXP_EvalDataStack* dataStack, const char* srcFile, const EXP_EvalNativeEnv* nativeEnv,
    bool traceSrcInfo
)
{
    EXP_EvalRet ret = { EXP_EvalErrCode_NONE };
    char* src = NULL;
    u32 srcSize = FILEU_readFile(srcFile, &src);
    if (-1 == srcSize)
    {
        ret.errCode = EXP_EvalErrCode_SrcFile;
        ret.errSrcFile = srcFile;
        return ret;
    }
    if (0 == srcSize)
    {
        return ret;
    }

    EXP_NodeSrcInfoTable* srcInfoTable = NULL;
    EXP_NodeSrcInfoTable _srcInfoTable = { 0 };
    if (traceSrcInfo)
    {
        srcInfoTable = &_srcInfoTable;
    }
    EXP_Node root = EXP_loadSrcAsList(space, src, srcInfoTable);
    free(src);
    if (EXP_NodeId_Invalid == root.id)
    {
        ret.errCode = EXP_EvalErrCode_ExpSyntax;
        ret.errSrcFile = srcFile;
        if (srcInfoTable)
        {
#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable : 6011)
#endif
            ret.errSrcFileLine = vec_last(srcInfoTable).line;
            ret.errSrcFileColumn = vec_last(srcInfoTable).column;
#ifdef _MSC_VER
# pragma warning(pop)
#endif
        }
        else
        {
            ret.errSrcFileLine = -1;
            ret.errSrcFileColumn = -1;
        }
        return ret;
    }
    ret = EXP_eval(space, dataStack, root, nativeEnv, srcInfoTable);
    if (srcInfoTable)
    {
        vec_free(srcInfoTable);
    }
    return ret;
}













static bool EXP_evalBoolFromStr(u32 len, const char* str, EXP_EvalValueData* pData)
{
    if (0 == strncmp(str, "true", len))
    {
        pData->b = true;
        return true;
    }
    if (0 == strncmp(str, "false", len))
    {
        pData->b = true;
        return true;
    }
    return false;
}

static bool EXP_evalNumFromStr(u32 len, const char* str, EXP_EvalValueData* pData)
{
    double num;
    u32 r = NSTR_str2num(&num, str, len, NULL);
    if (len == r)
    {
        pData->num = num;
    }
    return len == r;
}

const EXP_EvalValueTypeInfo EXP_EvalPrimValueTypeInfoTable[EXP_NumEvalPrimValueTypes] =
{
    { "bool", EXP_evalBoolFromStr },
    { "num", EXP_evalNumFromStr },
    { "tok" },
};













static void EXP_evalNativeFunCall_Add(EXP_Space* space, EXP_EvalValue* ins, EXP_EvalValueData* outs)
{
    double a = ins[0].data.num;
    double b = ins[1].data.num;
    outs[0].num = a + b;
}

static void EXP_evalNativeFunCall_Sub(EXP_Space* space, EXP_EvalValue* ins, EXP_EvalValueData* outs)
{
    double a = ins[0].data.num;
    double b = ins[1].data.num;
    outs[0].num = a - b;
}

static void EXP_evalNativeFunCall_Mul(EXP_Space* space, EXP_EvalValue* ins, EXP_EvalValueData* outs)
{
    double a = ins[0].data.num;
    double b = ins[1].data.num;
    outs[0].num = a * b;
}

static void EXP_evalNativeFunCall_Div(EXP_Space* space, EXP_EvalValue* ins, EXP_EvalValueData* outs)
{
    double a = ins[0].data.num;
    double b = ins[1].data.num;
    outs[0].num = a / b;
}






static void EXP_evalNativeFunCall_EQ(EXP_Space* space, EXP_EvalValue* ins, EXP_EvalValueData* outs)
{
    double a = ins[0].data.num;
    double b = ins[1].data.num;
    outs[0].b = a == b;
}

static void EXP_evalNativeFunCall_GT(EXP_Space* space, EXP_EvalValue* ins, EXP_EvalValueData* outs)
{
    double a = ins[0].data.num;
    double b = ins[1].data.num;
    outs[0].b = a > b;
}

static void EXP_evalNativeFunCall_LT(EXP_Space* space, EXP_EvalValue* ins, EXP_EvalValueData* outs)
{
    double a = ins[0].data.num;
    double b = ins[1].data.num;
    outs[0].b = a < b;
}

static void EXP_evalNativeFunCall_GE(EXP_Space* space, EXP_EvalValue* ins, EXP_EvalValueData* outs)
{
    double a = ins[0].data.num;
    double b = ins[1].data.num;
    outs[0].b = a >= b;
}

static void EXP_evalNativeFunCall_LE(EXP_Space* space, EXP_EvalValue* ins, EXP_EvalValueData* outs)
{
    double a = ins[0].data.num;
    double b = ins[1].data.num;
    outs[0].b = a <= b;
}




const EXP_EvalNativeFunInfo EXP_EvalPrimFunInfoTable[EXP_NumEvalPrimFuns] =
{
    { "blk" },
    { "def" },
    { "if" },

    {
        "+",
        EXP_evalNativeFunCall_Add,
        2, { EXP_EvalPrimValueType_Num, EXP_EvalPrimValueType_Num },
        1, { EXP_EvalPrimValueType_Num },
    },
    {
        "-",
        EXP_evalNativeFunCall_Sub,
        2, { EXP_EvalPrimValueType_Num, EXP_EvalPrimValueType_Num },
        1, { EXP_EvalPrimValueType_Num },
    },
    {
        "*",
        EXP_evalNativeFunCall_Mul,
        2, { EXP_EvalPrimValueType_Num, EXP_EvalPrimValueType_Num },
        1, { EXP_EvalPrimValueType_Num },
    },
    {
        "/",
        EXP_evalNativeFunCall_Div,
        2, { EXP_EvalPrimValueType_Num, EXP_EvalPrimValueType_Num },
        1, { EXP_EvalPrimValueType_Num },
    },

    {
        "=",
        EXP_evalNativeFunCall_EQ,
        2, { EXP_EvalPrimValueType_Num, EXP_EvalPrimValueType_Num },
        1, { EXP_EvalPrimValueType_Bool },
    },
    {
        ">",
        EXP_evalNativeFunCall_GT,
        2, { EXP_EvalPrimValueType_Num, EXP_EvalPrimValueType_Num },
        1, { EXP_EvalPrimValueType_Bool },
    },
    {
        "<",
        EXP_evalNativeFunCall_LT,
        2, { EXP_EvalPrimValueType_Num, EXP_EvalPrimValueType_Num },
        1, { EXP_EvalPrimValueType_Bool },
    },
    {
        ">=",
        EXP_evalNativeFunCall_GE,
        2, { EXP_EvalPrimValueType_Num, EXP_EvalPrimValueType_Num },
        1, { EXP_EvalPrimValueType_Bool },
    },
    {
        "<=",
        EXP_evalNativeFunCall_LE,
        2, { EXP_EvalPrimValueType_Num, EXP_EvalPrimValueType_Num },
        1, { EXP_EvalPrimValueType_Bool },
    },
};















































































































