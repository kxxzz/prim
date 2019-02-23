#include "exp_eval_a.h"



typedef struct EXP_EvalVerifVar
{
    u32 valType;
    EXP_Node block;
    u32 id;
} EXP_EvalVerifVar;

typedef struct EXP_EvalVerifDef
{
    EXP_Node key;
    bool isVar;
    union
    {
        EXP_Node fun;
        EXP_EvalVerifVar var;
    };
} EXP_EvalVerifDef;

typedef vec_t(EXP_EvalVerifDef) EXP_EvalVerifDefTable;



typedef enum EXP_EvalVerifBlockTypeInferState
{
    EXP_EvalVerifBlockTypeInferState_None = 0,
    EXP_EvalVerifBlockTypeInferState_Entered,
    EXP_EvalVerifBlockTypeInferState_Done,
} EXP_EvalVerifBlockTypeInferState;

typedef struct EXP_EvalVerifOutType
{
    u32 id;
    bool isVar;
} EXP_EvalVerifOutType;

typedef vec_t(EXP_EvalVerifOutType) EXP_EvalVerifOutTypeVec;

typedef struct EXP_EvalVerifBlock
{
    EXP_Node parent;
    EXP_EvalVerifDefTable defs;
    u32 varsCount;

    EXP_EvalVerifBlockTypeInferState typeInferState;
    vec_u32 typeIn;
    EXP_EvalVerifOutTypeVec typeOut;
} EXP_EvalVerifBlock;

typedef vec_t(EXP_EvalVerifBlock) EXP_EvalVerifBlockTable;

static void EXP_evalVerifBlockFree(EXP_EvalVerifBlock* info)
{
    vec_free(&info->typeOut);
    vec_free(&info->typeIn);
    vec_free(&info->defs);
}

static void EXP_evalVerifBlockReset(EXP_EvalVerifBlock* info)
{
    info->parent = EXP_Node_Invalid;
    info->defs.length = 0;

    info->typeInferState = EXP_EvalVerifBlockTypeInferState_None;
    info->typeIn.length = 0;
    info->typeOut.length = 0;
}





typedef enum EXP_EvalVerifBlockCallbackType
{
    EXP_EvalVerifBlockCallbackType_NONE,
    EXP_EvalVerifBlockCallbackType_NativeCall,
    EXP_EvalVerifBlockCallbackType_Call,
    EXP_EvalVerifBlockCallbackType_Cond,
    EXP_EvalVerifBlockCallbackType_Branch0,
    EXP_EvalVerifBlockCallbackType_Branch1,
    EXP_EvalVerifBlockCallbackType_BranchUnify,
} EXP_EvalVerifBlockCallbackType;

typedef struct EXP_EvalVerifBlockCallback
{
    EXP_EvalVerifBlockCallbackType type;
    union
    {
        u32 nativeFun;
        EXP_Node fun;
    };
} EXP_EvalVerifBlockCallback;

static EXP_EvalVerifBlockCallback EXP_EvalBlockCallback_NONE = { EXP_EvalVerifBlockCallbackType_NONE };





typedef struct EXP_EvalVerifCall
{
    EXP_Node srcNode;
    u32 dataStackP;
    EXP_Node* p;
    EXP_Node* end;
    EXP_EvalVerifBlockCallback cb;
} EXP_EvalVerifCall;

typedef vec_t(EXP_EvalVerifCall) EXP_EvalVerifCallStack;





typedef struct EXP_EvalVerifContext
{
    EXP_Space* space;
    EXP_EvalValueTypeInfoTable* valueTypeTable;
    EXP_EvalNativeFunInfoTable* nativeFunTable;
    EXP_EvalNodeTable* nodeTable;
    EXP_SpaceSrcInfo* srcInfo;

    u32 blockTableBase;
    EXP_EvalVerifBlockTable blockTable;
    vec_u32 dataStack;
    EXP_EvalVerifCallStack callStack;
    EXP_NodeVec recheckNodes;
    bool recheckFlag;
    bool dataStackShiftEnable;
    EXP_EvalError error;
    EXP_NodeVec varKeyBuf;
} EXP_EvalVerifContext;





static EXP_EvalVerifContext EXP_newEvalVerifContext
(
    EXP_Space* space,
    EXP_EvalValueTypeInfoTable* valueTypeTable,
    EXP_EvalNativeFunInfoTable* nativeFunTable,
    EXP_EvalNodeTable* nodeTable,
    EXP_SpaceSrcInfo* srcInfo
)
{
    EXP_EvalVerifContext _ctx = { 0 };
    EXP_EvalVerifContext* ctx = &_ctx;
    ctx->space = space;
    ctx->valueTypeTable = valueTypeTable;
    ctx->nativeFunTable = nativeFunTable;
    ctx->nodeTable = nodeTable;
    ctx->srcInfo = srcInfo;

    u32 n = EXP_spaceNodesTotal(space);
    u32 nodeTableLength0 = nodeTable->length;
    vec_resize(nodeTable, n);
    memset(nodeTable->data + nodeTableLength0, 0, sizeof(EXP_EvalNode)*n);

    ctx->blockTableBase = nodeTableLength0;
    vec_resize(&ctx->blockTable, n);
    memset(ctx->blockTable.data, 0, sizeof(EXP_EvalVerifBlock)*ctx->blockTable.length);
    return *ctx;
}

static void EXP_evalVerifContextFree(EXP_EvalVerifContext* ctx)
{
    vec_free(&ctx->varKeyBuf);
    vec_free(&ctx->recheckNodes);
    vec_free(&ctx->callStack);
    vec_free(&ctx->dataStack);
    for (u32 i = 0; i < ctx->blockTable.length; ++i)
    {
        EXP_EvalVerifBlock* b = ctx->blockTable.data + i;
        EXP_evalVerifBlockFree(b);
    }
    vec_free(&ctx->blockTable);
}






static void EXP_evalVerifErrorAtNode(EXP_EvalVerifContext* ctx, EXP_Node node, EXP_EvalErrCode errCode)
{
    ctx->error.code = errCode;
    EXP_SpaceSrcInfo* srcInfo = ctx->srcInfo;
    if (srcInfo)
    {
        assert(node.id < srcInfo->nodes.length);
        ctx->error.file = srcInfo->nodes.data[node.id].file;
        ctx->error.line = srcInfo->nodes.data[node.id].line;
        ctx->error.column = srcInfo->nodes.data[node.id].column;
    }
}








static EXP_EvalVerifBlock* EXP_evalVerifGetBlock(EXP_EvalVerifContext* ctx, EXP_Node node)
{
    assert(node.id >= ctx->blockTableBase);
    EXP_EvalVerifBlock* b = ctx->blockTable.data + node.id - ctx->blockTableBase;
    return b;
}





static bool EXP_evalVerifGetMatched
(
    EXP_EvalVerifContext* ctx, const char* name, EXP_Node blkNode, EXP_EvalVerifDef* outDef
)
{
    EXP_Space* space = ctx->space;
    while (blkNode.id != EXP_NodeId_Invalid)
    {
        EXP_EvalVerifBlock* blk = EXP_evalVerifGetBlock(ctx, blkNode);
        for (u32 i = 0; i < blk->defs.length; ++i)
        {
            EXP_EvalVerifDef* def = blk->defs.data + blk->defs.length - 1 - i;
            const char* str = EXP_tokCstr(space, def->key);
            if (0 == strcmp(str, name))
            {
                *outDef = *def;
                return true;
            }
        }
        blkNode = blk->parent;
    }
    return false;
}




static u32 EXP_evalVerifGetNativeFun(EXP_EvalVerifContext* ctx, const char* name)
{
    EXP_Space* space = ctx->space;
    for (u32 i = 0; i < ctx->nativeFunTable->length; ++i)
    {
        u32 idx = ctx->nativeFunTable->length - 1 - i;
        const char* s = ctx->nativeFunTable->data[idx].name;
        if (0 == strcmp(s, name))
        {
            return idx;
        }
    }
    return -1;
}




static EXP_EvalKey EXP_evalVerifGetKey(EXP_EvalVerifContext* ctx, const char* name)
{
    for (EXP_EvalKey i = 0; i < EXP_NumEvalKeys; ++i)
    {
        EXP_EvalKey k = EXP_NumEvalKeys - 1 - i;
        const char* s = EXP_EvalKeyNameTable[k];
        if (0 == strcmp(s, name))
        {
            return k;
        }
    }
    return -1;
}










static void EXP_evalVerifLoadDef(EXP_EvalVerifContext* ctx, EXP_Node node, EXP_EvalVerifBlock* blk)
{
    EXP_Space* space = ctx->space;
    EXP_EvalVerifBlockTable* blockTable = &ctx->blockTable;
    if (EXP_isTok(space, node))
    {
        return;
    }
    if (!EXP_evalCheckCall(space, node))
    {
        EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalSyntax);
        return;
    }
    EXP_Node* defCall = EXP_seqElm(space, node);
    const char* kDef = EXP_tokCstr(space, defCall[0]);
    EXP_EvalKey k = EXP_evalVerifGetKey(ctx, kDef);
    if (k != EXP_EvalKey_Def)
    {
        return;
    }
    EXP_Node name;
    if (EXP_isTok(space, defCall[1]))
    {
        name = defCall[1];
    }
    else
    {
        EXP_evalVerifErrorAtNode(ctx, defCall[1], EXP_EvalErrCode_EvalSyntax);
        return;
    }
    EXP_EvalVerifDef def = { name, false, .fun = node };
    vec_push(&blk->defs, def);
}









static void EXP_evalVerifDefGetBody(EXP_EvalVerifContext* ctx, EXP_Node node, u32* pLen, EXP_Node** pSeq)
{
    EXP_Space* space = ctx->space;
    assert(EXP_seqLen(space, node) >= 2);
    *pLen = EXP_seqLen(space, node) - 2;
    EXP_Node* defCall = EXP_seqElm(space, node);
    *pSeq = defCall + 2;
}











static void EXP_evalVerifEnterBlock
(
    EXP_EvalVerifContext* ctx, EXP_Node* seq, u32 len, EXP_Node srcNode, EXP_Node parent, EXP_EvalVerifBlockCallback cb,
    bool isDefScope
)
{
    u32 dataStackP = ctx->dataStack.length;
    EXP_EvalVerifCall call = { srcNode, dataStackP, seq, seq + len, cb };
    vec_push(&ctx->callStack, call);

    EXP_EvalVerifBlock* blk = EXP_evalVerifGetBlock(ctx, srcNode);
    if (ctx->recheckFlag && (blk->typeInferState != EXP_EvalVerifBlockTypeInferState_None))
    {
        assert(blk->parent.id == parent.id);
        EXP_evalVerifBlockReset(blk);
    }
    assert(EXP_EvalVerifBlockTypeInferState_None == blk->typeInferState);
    blk->typeInferState = EXP_EvalVerifBlockTypeInferState_Entered;
    blk->parent = parent;
    if (isDefScope)
    {
        for (u32 i = 0; i < len; ++i)
        {
            EXP_evalVerifLoadDef(ctx, seq[i], blk);
            if (ctx->error.code)
            {
                return;
            }
        }
    }
}



static void EXP_evalVerifLeaveBlock(EXP_EvalVerifContext* ctx)
{
    EXP_EvalVerifCall* curCall = &vec_last(&ctx->callStack);
    EXP_EvalVerifBlock* curBlock = EXP_evalVerifGetBlock(ctx, curCall->srcNode);

    if (ctx->recheckFlag && (1 == ctx->callStack.length))
    {
        assert(EXP_EvalVerifBlockTypeInferState_Done == curBlock->typeInferState);
        vec_pop(&ctx->callStack);
        return;
    }
    assert(ctx->dataStack.length + curBlock->typeIn.length >= curCall->dataStackP);
    u32 numOuts = ctx->dataStack.length + curBlock->typeIn.length - curCall->dataStackP;
    for (u32 i = 0; i < numOuts; ++i)
    {
        u32 j = ctx->dataStack.length - numOuts + i;
        // todo
        EXP_EvalVerifOutType t = { ctx->dataStack.data[j] };
        vec_push(&curBlock->typeOut, t);
    }

    assert(EXP_EvalVerifBlockTypeInferState_Entered == curBlock->typeInferState);
    curBlock->typeInferState = EXP_EvalVerifBlockTypeInferState_Done;

    vec_pop(&ctx->callStack);
}



static void EXP_evalVerifBlockSaveInfo(EXP_EvalVerifContext* ctx, EXP_EvalVerifBlock* nodeInfo)
{
    EXP_EvalVerifCall* curCall = &vec_last(&ctx->callStack);
    EXP_EvalVerifBlock* curBlock = EXP_evalVerifGetBlock(ctx, curCall->srcNode);
    assert(ctx->dataStack.length + curBlock->typeIn.length >= curCall->dataStackP);

    if (EXP_EvalVerifBlockTypeInferState_Done == nodeInfo->typeInferState)
    {
        return;
    }
    assert(0 == nodeInfo->typeIn.length);
    for (u32 i = 0; i < curBlock->typeIn.length; ++i)
    {
        vec_push(&nodeInfo->typeIn, curBlock->typeIn.data[i]);
    }
    u32 numOuts = ctx->dataStack.length + curBlock->typeIn.length - curCall->dataStackP;
    for (u32 i = 0; i < numOuts; ++i)
    {
        u32 j = ctx->dataStack.length - numOuts + i;
        // todo
        EXP_EvalVerifOutType t = { ctx->dataStack.data[j] };
        vec_push(&nodeInfo->typeOut, t);
    }
    nodeInfo->typeInferState = EXP_EvalVerifBlockTypeInferState_Done;
}



static void EXP_evalVerifBlockRebase(EXP_EvalVerifContext* ctx)
{
    EXP_EvalVerifCall* curCall = &vec_last(&ctx->callStack);
    EXP_EvalVerifBlock* curBlock = EXP_evalVerifGetBlock(ctx, curCall->srcNode);
    assert(0 == curBlock->typeOut.length);

    assert(curCall->dataStackP >= curBlock->typeIn.length);
    ctx->dataStack.length = curCall->dataStackP - curBlock->typeIn.length;
    for (u32 i = 0; i < curBlock->typeIn.length; ++i)
    {
        vec_push(&ctx->dataStack, curBlock->typeIn.data[i]);
    }
}

static void EXP_evalVerifCancelBlock(EXP_EvalVerifContext* ctx)
{
    EXP_evalVerifBlockRebase(ctx);

    EXP_EvalVerifCall* curCall = &vec_last(&ctx->callStack);
    EXP_EvalVerifBlock* curBlock = EXP_evalVerifGetBlock(ctx, curCall->srcNode);
    assert(EXP_EvalVerifBlockTypeInferState_Entered == curBlock->typeInferState);
    EXP_evalVerifBlockReset(curBlock);

    vec_pop(&ctx->callStack);
}






static bool EXP_evalVerifShiftDataStack(EXP_EvalVerifContext* ctx, u32 n, const u32* a)
{
    if (!ctx->dataStackShiftEnable)
    {
        return false;
    }
    vec_u32* dataStack = &ctx->dataStack;
    vec_insertarr(dataStack, 0, a, n);
    for (u32 i = 0; i < ctx->callStack.length; ++i)
    {
        EXP_EvalVerifCall* c = ctx->callStack.data + i;
        c->dataStackP += n;
    }
    return true;
}









static void EXP_evalVerifCurBlockInsUpdate(EXP_EvalVerifContext* ctx, u32 argsOffset, const u32* funInTypes)
{
    EXP_EvalVerifCall* curCall = &vec_last(&ctx->callStack);
    EXP_EvalVerifBlock* curBlock = EXP_evalVerifGetBlock(ctx, curCall->srcNode);
    assert(EXP_EvalVerifBlockTypeInferState_Entered == curBlock->typeInferState);
    assert(curCall->dataStackP >= curBlock->typeIn.length);
    if (curCall->dataStackP > argsOffset + curBlock->typeIn.length)
    {
        u32 n = curCall->dataStackP - argsOffset;
        assert(n > curBlock->typeIn.length);
        u32 added = n - curBlock->typeIn.length;
        for (u32 i = 0; i < added; ++i)
        {
            vec_insert(&curBlock->typeIn, i, funInTypes[i]);
        }
    }
}






static void EXP_evalVerifNativeFunCall(EXP_EvalVerifContext* ctx, EXP_EvalNativeFunInfo* nativeFunInfo, EXP_Node srcNode)
{
    EXP_Space* space = ctx->space;
    vec_u32* dataStack = &ctx->dataStack;

    assert(dataStack->length >= nativeFunInfo->numIns);
    u32 argsOffset = dataStack->length - nativeFunInfo->numIns;
    EXP_evalVerifCurBlockInsUpdate(ctx, argsOffset, nativeFunInfo->inType);

    for (u32 i = 0; i < nativeFunInfo->numIns; ++i)
    {
        u32 vt1 = dataStack->data[argsOffset + i];
        u32 vt = nativeFunInfo->inType[i];
        if (!EXP_evalTypeMatch(vt, vt1))
        {
            EXP_evalVerifErrorAtNode(ctx, srcNode, EXP_EvalErrCode_EvalArgs);
            return;
        }
    }
    vec_resize(dataStack, argsOffset);
    for (u32 i = 0; i < nativeFunInfo->numOuts; ++i)
    {
        vec_push(dataStack, nativeFunInfo->outType[i]);
    }
}




static void EXP_evalVerifFunCall(EXP_EvalVerifContext* ctx, const EXP_EvalVerifBlock* funInfo, EXP_Node srcNode)
{
    EXP_Space* space = ctx->space;
    vec_u32* dataStack = &ctx->dataStack;

    assert(dataStack->length >= funInfo->typeIn.length);
    u32 argsOffset = dataStack->length - funInfo->typeIn.length;
    EXP_evalVerifCurBlockInsUpdate(ctx, argsOffset, funInfo->typeIn.data);

    for (u32 i = 0; i < funInfo->typeIn.length; ++i)
    {
        u32 vt1 = dataStack->data[argsOffset + i];
        u32 vt = funInfo->typeIn.data[i];
        if (!EXP_evalTypeMatch(vt, vt1))
        {
            EXP_evalVerifErrorAtNode(ctx, srcNode, EXP_EvalErrCode_EvalArgs);
            return;
        }
    }
    vec_resize(dataStack, argsOffset);
    for (u32 i = 0; i < funInfo->typeOut.length; ++i)
    {
        EXP_EvalVerifOutType t = funInfo->typeOut.data[i];
        if (t.isVar)
        {
            assert(t.id < funInfo->typeIn.length);
            u32 vt = funInfo->typeIn.data[t.id];
            vec_push(dataStack, vt);
        }
        else
        {
            vec_push(dataStack, t.id);
        }
    }
}










static void EXP_evalVerifAddRecheck(EXP_EvalVerifContext* ctx, EXP_Node node)
{
    vec_push(&ctx->recheckNodes, node);
}


static void EXP_evalVerifRecurFun
(
    EXP_EvalVerifContext* ctx, EXP_EvalVerifCall* curCall, const EXP_EvalVerifBlock* funInfo
)
{
    assert(EXP_EvalVerifBlockTypeInferState_Entered == funInfo->typeInferState);
    assert(!ctx->recheckFlag);
    EXP_Space* space = ctx->space;

    EXP_Node lastSrcNode = EXP_Node_Invalid;
    while (ctx->callStack.length > 0)
    {
        lastSrcNode = curCall->srcNode;
        curCall = &vec_last(&ctx->callStack);
        EXP_Node srcNode = curCall->srcNode;
        EXP_EvalVerifBlockCallback* cb = &curCall->cb;
        // quit this branch until recheck pass
        if (EXP_EvalVerifBlockCallbackType_Branch0 == cb->type)
        {
            if (EXP_evalIfHasBranch1(space, srcNode))
            {
                EXP_evalVerifBlockRebase(ctx);
                curCall->p = EXP_evalIfBranch1(space, srcNode);
                curCall->end = EXP_evalIfBranch1(space, srcNode) + 1;
                cb->type = EXP_EvalVerifBlockCallbackType_NONE;
                EXP_evalVerifAddRecheck(ctx, srcNode);
                return;
            }
        }
        else if (EXP_EvalVerifBlockCallbackType_BranchUnify == cb->type)
        {
            EXP_evalVerifBlockRebase(ctx);
            curCall->p = EXP_evalIfBranch0(space, srcNode);
            curCall->end = EXP_evalIfBranch0(space, srcNode) + 1;
            cb->type = EXP_EvalVerifBlockCallbackType_NONE;
            EXP_evalVerifAddRecheck(ctx, srcNode);
            return;
        }
        EXP_evalVerifCancelBlock(ctx);
    }
    EXP_evalVerifErrorAtNode(ctx, lastSrcNode, EXP_EvalErrCode_EvalRecurNoBaseCase);
}






static void EXP_evalVerifNode
(
    EXP_EvalVerifContext* ctx, EXP_Node node, EXP_EvalVerifCall* curCall, EXP_EvalVerifBlock* curBlock
)
{
    EXP_Space* space = ctx->space;
    EXP_EvalNodeTable* nodeTable = ctx->nodeTable;
    EXP_EvalVerifBlockTable* blockTable = &ctx->blockTable;
    vec_u32* dataStack = &ctx->dataStack;

    EXP_EvalNode* enode = nodeTable->data + node.id;
    if (EXP_isTok(space, node))
    {
        const char* name = EXP_tokCstr(space, node);

        EXP_EvalKey k = EXP_evalVerifGetKey(ctx, name);
        switch (k)
        {
        case EXP_EvalKey_VarDefBegin:
        {
            enode->type = EXP_EvalNodeType_VarDefBegin;
            if (curCall->cb.type != EXP_EvalVerifBlockCallbackType_NONE)
            {
                EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                return;
            }
            ctx->varKeyBuf.length = 0;
            for (u32 n = 0;;)
            {
                if (curCall->p == curCall->end)
                {
                    EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalSyntax);
                    return;
                }
                node = *(curCall->p++);
                enode = nodeTable->data + node.id;
                const char* skey = EXP_tokCstr(space, node);
                if (!EXP_isTok(space, node))
                {
                    EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                    return;
                }
                EXP_EvalKey k = EXP_evalVerifGetKey(ctx, EXP_tokCstr(space, node));
                if (k != -1)
                {
                    if (EXP_EvalKey_VarDefEnd == k)
                    {
                        enode->type = EXP_EvalNodeType_VarDefEnd;
                        if (n > dataStack->length)
                        {
                            for (u32 i = 0; i < n - dataStack->length; ++i)
                            {
                                u32 a[] = { EXP_EvalValueType_Any };
                                if (!EXP_evalVerifShiftDataStack(ctx, 1, a))
                                {
                                    EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                                    return;
                                }
                            }
                        }

                        u32 off = dataStack->length - n;
                        for (u32 i = 0; i < n; ++i)
                        {
                            u32 vt = dataStack->data[off + i];
                            EXP_EvalVerifVar var = { vt, curCall->srcNode.id, curBlock->varsCount };
                            EXP_EvalVerifDef def = { ctx->varKeyBuf.data[i], true, .var = var };
                            vec_push(&curBlock->defs, def);
                            ++curBlock->varsCount;
                        }
                        assert(n <= dataStack->length);
                        vec_resize(dataStack, off);
                        ctx->varKeyBuf.length = 0;

                        if (curCall->dataStackP > dataStack->length + curBlock->typeIn.length)
                        {
                            u32 n = curCall->dataStackP - dataStack->length - curBlock->typeIn.length;
                            u32 added = n - curBlock->typeIn.length;
                            for (u32 i = 0; i < added; ++i)
                            {
                                EXP_EvalVerifDef* def = curBlock->defs.data + curBlock->defs.length - added + i;
                                assert(def->isVar);
                                vec_insert(&curBlock->typeIn, i, EXP_EvalValueType_Any);
                            }
                        }
                        return;
                    }
                    EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                    return;
                }
                vec_push(&ctx->varKeyBuf, node);
                ++n;
            }
        }
        case EXP_EvalKey_Drop:
        {
            enode->type = EXP_EvalNodeType_Drop;
            if (!dataStack->length)
            {
                u32 a[] = { EXP_EvalValueType_Any };
                if (!EXP_evalVerifShiftDataStack(ctx, 1, a))
                {
                    EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                    return;
                }
            }
            vec_pop(dataStack);
            if (curCall->dataStackP > dataStack->length + curBlock->typeIn.length)
            {
                u32 n = curCall->dataStackP - dataStack->length;
                u32 added = n - curBlock->typeIn.length;
                assert(1 == added);
                vec_insert(&curBlock->typeIn, 0, EXP_EvalValueType_Any);
            }
            return;
        }
        default:
            break;
        }

        u32 nativeFun = EXP_evalVerifGetNativeFun(ctx, name);
        if (nativeFun != -1)
        {
            enode->type = EXP_EvalNodeType_NativeFun;
            enode->nativeFun = nativeFun;
            EXP_EvalNativeFunInfo* nativeFunInfo = ctx->nativeFunTable->data + nativeFun;
            if (!nativeFunInfo->call)
            {
                EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                return;
            }
            if (dataStack->length < nativeFunInfo->numIns)
            {
                u32 n = nativeFunInfo->numIns - dataStack->length;
                if (!EXP_evalVerifShiftDataStack(ctx, n, nativeFunInfo->inType))
                {
                    EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                    return;
                }
            }
            EXP_evalVerifNativeFunCall(ctx, nativeFunInfo, node);
            return;
        }

        EXP_EvalVerifDef def = { 0 };
        if (EXP_evalVerifGetMatched(ctx, name, curCall->srcNode, &def))
        {
            if (def.isVar)
            {
                enode->type = EXP_EvalNodeType_Var;
                enode->var.block = def.var.block;
                enode->var.id = def.var.id;
                vec_push(dataStack, def.var.valType);
                return;
            }
            else
            {
                enode->type = EXP_EvalNodeType_Fun;
                enode->funDef = def.fun;
                EXP_Node fun = def.fun;
                EXP_EvalVerifBlock* funInfo = blockTable->data + fun.id;
                if (EXP_EvalVerifBlockTypeInferState_Done == funInfo->typeInferState)
                {
                    if (dataStack->length < funInfo->typeIn.length)
                    {
                        u32 n = funInfo->typeIn.length - dataStack->length;
                        if (!EXP_evalVerifShiftDataStack(ctx, n, funInfo->typeIn.data))
                        {
                            EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
                            return;
                        }
                    }
                    EXP_evalVerifFunCall(ctx, funInfo, node);
                    return;
                }
                else if (EXP_EvalVerifBlockTypeInferState_None == funInfo->typeInferState)
                {
                    u32 bodyLen = 0;
                    EXP_Node* body = NULL;
                    EXP_evalVerifDefGetBody(ctx, fun, &bodyLen, &body);
                    EXP_evalVerifEnterBlock
                    (
                        ctx, body, bodyLen, fun, curCall->srcNode, EXP_EvalBlockCallback_NONE, true
                    );
                    if (ctx->error.code)
                    {
                        return;
                    }
                    return;
                }
                else
                {
                    EXP_evalVerifRecurFun(ctx, curCall, funInfo);
                    if (ctx->error.code)
                    {
                        return;
                    }
                    return;
                }
            }
        }
        else
        {
            bool isQuoted = EXP_tokQuoted(space, node);
            if (!isQuoted)
            {
                for (u32 i = 0; i < ctx->valueTypeTable->length; ++i)
                {
                    u32 j = ctx->valueTypeTable->length - 1 - i;
                    if (ctx->valueTypeTable->data[j].ctorBySym)
                    {
                        u32 l = EXP_tokSize(space, node);
                        const char* s = EXP_tokCstr(space, node);
                        EXP_EvalValue v = { 0 };
                        if (ctx->valueTypeTable->data[j].ctorBySym(l, s, &v))
                        {
                            enode->type = EXP_EvalNodeType_Value;
                            enode->value = v;
                            vec_push(dataStack, j);
                            return;
                        }
                    }
                }
                EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalUndefined);
                return;
            }
            enode->type = EXP_EvalNodeType_String;
            vec_push(dataStack, EXP_EvalPrimValueType_STRING);
            return;
        }
    }

    if (!EXP_evalCheckCall(space, node))
    {
        EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalSyntax);
        return;
    }

    EXP_Node* elms = EXP_seqElm(space, node);
    u32 len = EXP_seqLen(space, node);
    const char* name = EXP_tokCstr(space, elms[0]);

    EXP_EvalVerifDef def = { 0 };
    if (EXP_evalVerifGetMatched(ctx, name, curCall->srcNode, &def))
    {
        if (def.isVar)
        {
            enode->type = EXP_EvalNodeType_CallVar;
            enode->var.block = def.var.block;
            enode->var.id = def.var.id;
            vec_push(dataStack, def.var.valType);
            return;
        }
        else
        {
            enode->type = EXP_EvalNodeType_CallFun;
            enode->funDef = def.fun;

            EXP_EvalVerifBlockCallback cb = { EXP_EvalVerifBlockCallbackType_Call, .fun = def.fun };
            EXP_evalVerifEnterBlock(ctx, elms + 1, len - 1, node, curCall->srcNode, cb, false);
            return;
        }
    }

    u32 nativeFun = EXP_evalVerifGetNativeFun(ctx, name);
    if (nativeFun != -1)
    {
        enode->type = EXP_EvalNodeType_CallNativeFun;
        enode->nativeFun = nativeFun;
        EXP_EvalNativeFunInfo* nativeFunInfo = ctx->nativeFunTable->data + nativeFun;
        assert(nativeFunInfo->call);
        EXP_EvalVerifBlockCallback cb = { EXP_EvalVerifBlockCallbackType_NativeCall,.nativeFun = nativeFun };
        EXP_evalVerifEnterBlock(ctx, elms + 1, len - 1, node, curCall->srcNode, cb, false);
        return;
    }

    EXP_EvalKey k = EXP_evalVerifGetKey(ctx, name);
    switch (k)
    {
    case EXP_EvalKey_Def:
    {
        enode->type = EXP_EvalNodeType_Def;
        return;
    }
    case EXP_EvalKey_If:
    {
        enode->type = EXP_EvalNodeType_If;
        if ((len != 3) && (len != 4))
        {
            EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalArgs);
            return;
        }
        EXP_EvalVerifBlockCallback cb = { EXP_EvalVerifBlockCallbackType_Cond };
        EXP_evalVerifEnterBlock(ctx, elms + 1, 1, node, curCall->srcNode, cb, false);
        return;
    }
    default:
    {
        EXP_evalVerifErrorAtNode(ctx, node, EXP_EvalErrCode_EvalSyntax);
        return;
    }
    }
}






static void EXP_evalVerifCall(EXP_EvalVerifContext* ctx)
{
    EXP_Space* space = ctx->space;
    EXP_EvalVerifCall* curCall;
    EXP_EvalVerifBlockTable* blockTable = &ctx->blockTable;
    EXP_EvalVerifBlock* curBlock = NULL;
    vec_u32* dataStack = &ctx->dataStack;
next:
    if (ctx->error.code)
    {
        return;
    }
    if (0 == ctx->callStack.length)
    {
        return;
    }
    curCall = &vec_last(&ctx->callStack);
    curBlock = blockTable->data + curCall->srcNode.id;
    if (curCall->p == curCall->end)
    {
        EXP_EvalVerifBlockCallback* cb = &curCall->cb;
        switch (cb->type)
        {
        case EXP_EvalVerifBlockCallbackType_NONE:
        {
            EXP_evalVerifLeaveBlock(ctx);
            goto next;
        }
        case EXP_EvalVerifBlockCallbackType_NativeCall:
        {
            if (curBlock->typeIn.length > 0)
            {
                EXP_evalVerifErrorAtNode(ctx, curCall->srcNode, EXP_EvalErrCode_EvalArgs);
                goto next;
            }
            if (dataStack->length < curCall->dataStackP)
            {
                EXP_evalVerifErrorAtNode(ctx, curCall->srcNode, EXP_EvalErrCode_EvalArgs);
                goto next;
            }
            EXP_EvalNativeFunInfo* nativeFunInfo = ctx->nativeFunTable->data + cb->nativeFun;
            u32 numIns = dataStack->length - curCall->dataStackP;
            if (numIns != nativeFunInfo->numIns)
            {
                EXP_evalVerifErrorAtNode(ctx, curCall->srcNode, EXP_EvalErrCode_EvalArgs);
                goto next;
            }
            EXP_evalVerifNativeFunCall(ctx, nativeFunInfo, curCall->srcNode);
            EXP_evalVerifLeaveBlock(ctx);
            goto next;
        }
        case EXP_EvalVerifBlockCallbackType_Call:
        {
            EXP_Node srcNode = curCall->srcNode;
            EXP_Node fun = cb->fun;
            EXP_EvalVerifBlock* funInfo = blockTable->data + fun.id;
            if (EXP_EvalVerifBlockTypeInferState_Done == funInfo->typeInferState)
            {
                if (curBlock->typeIn.length > 0)
                {
                    EXP_evalVerifErrorAtNode(ctx, srcNode, EXP_EvalErrCode_EvalArgs);
                    goto next;
                }
                if (curCall->dataStackP != (dataStack->length - funInfo->typeIn.length))
                {
                    EXP_evalVerifErrorAtNode(ctx, srcNode, EXP_EvalErrCode_EvalArgs);
                    goto next;
                }
                EXP_evalVerifFunCall(ctx, funInfo, srcNode);
                EXP_evalVerifLeaveBlock(ctx);
                goto next;
            }
            else if (EXP_EvalVerifBlockTypeInferState_None == funInfo->typeInferState)
            {
                if (curCall->dataStackP > dataStack->length)
                {
                    EXP_evalVerifErrorAtNode(ctx, srcNode, EXP_EvalErrCode_EvalArgs);
                    goto next;
                }
                u32 bodyLen = 0;
                EXP_Node* body = NULL;
                EXP_evalVerifDefGetBody(ctx, fun, &bodyLen, &body);
                curCall->cb.type = EXP_EvalVerifBlockCallbackType_NONE;
                EXP_evalVerifEnterBlock(ctx, body, bodyLen, fun, srcNode, EXP_EvalBlockCallback_NONE, true);
                goto next;
            }
            else
            {
                EXP_evalVerifRecurFun(ctx, curCall, funInfo);
                goto next;
            }
            return;
        }
        case EXP_EvalVerifBlockCallbackType_Cond:
        {
            EXP_Node srcNode = curCall->srcNode;
            if (curCall->dataStackP + 1 != dataStack->length)
            {
                EXP_evalVerifErrorAtNode(ctx, curCall->srcNode, EXP_EvalErrCode_EvalArgs);
                goto next;
            }
            u32 vt = dataStack->data[curCall->dataStackP];
            vec_pop(dataStack);
            if (!EXP_evalTypeMatch(EXP_EvalPrimValueType_BOOL, vt))
            {
                EXP_evalVerifErrorAtNode(ctx, curCall->srcNode, EXP_EvalErrCode_EvalArgs);
                goto next;
            }
            curCall->p = EXP_evalIfBranch0(space, srcNode);
            curCall->end = EXP_evalIfBranch0(space, srcNode) + 1;
            cb->type = EXP_EvalVerifBlockCallbackType_Branch0;
            goto next;
        }
        case EXP_EvalVerifBlockCallbackType_Branch0:
        {
            EXP_Node srcNode = curCall->srcNode;
            EXP_EvalVerifBlock* b0 = blockTable->data + EXP_evalIfBranch0(space, srcNode)->id;
            EXP_evalVerifBlockSaveInfo(ctx, b0);
            assert(EXP_EvalVerifBlockTypeInferState_Done == b0->typeInferState);
            if (EXP_evalIfHasBranch1(space, srcNode))
            {
                if (dataStack->length < b0->typeOut.length)
                {
                    u32 n = b0->typeOut.length - dataStack->length;
                    if (!EXP_evalVerifShiftDataStack(ctx, n, b0->typeIn.data))
                    {
                        EXP_evalVerifErrorAtNode(ctx, *EXP_evalIfBranch0(space, srcNode), EXP_EvalErrCode_EvalArgs);
                        goto next;
                    }
                }
                for (u32 i = 0; i < b0->typeOut.length; ++i)
                {
                    vec_pop(dataStack);
                }
                for (u32 i = 0; i < b0->typeIn.length; ++i)
                {
                    vec_push(dataStack, b0->typeIn.data[i]);
                }
                curCall->p = EXP_evalIfBranch1(space, srcNode);
                curCall->end = EXP_evalIfBranch1(space, srcNode) + 1;
                cb->type = EXP_EvalVerifBlockCallbackType_BranchUnify;
                goto next;
            }
            EXP_evalVerifLeaveBlock(ctx);
            goto next;
        }
        case EXP_EvalVerifBlockCallbackType_BranchUnify:
        {
            EXP_Node srcNode = curCall->srcNode;
            assert(EXP_evalIfHasBranch1(space, srcNode));
            EXP_EvalVerifBlock* b0 = blockTable->data + EXP_evalIfBranch0(space, srcNode)->id;
            EXP_EvalVerifBlock* b1 = blockTable->data + EXP_evalIfBranch1(space, srcNode)->id;
            EXP_evalVerifBlockSaveInfo(ctx, b1);
            assert(EXP_EvalVerifBlockTypeInferState_Done == b0->typeInferState);
            assert(EXP_EvalVerifBlockTypeInferState_Done == b1->typeInferState);

            EXP_evalVerifLeaveBlock(ctx);
            assert(b1->typeIn.length == curBlock->typeIn.length);
            assert(b1->typeOut.length == curBlock->typeOut.length);

            if (b0->typeIn.length != b1->typeIn.length)
            {
                EXP_evalVerifErrorAtNode(ctx, curCall->srcNode, EXP_EvalErrCode_EvalBranchUneq);
                goto next;
            }
            if (b0->typeOut.length != b1->typeOut.length)
            {
                EXP_evalVerifErrorAtNode(ctx, curCall->srcNode, EXP_EvalErrCode_EvalBranchUneq);
                goto next;
            }
            for (u32 i = 0; i < b0->typeIn.length; ++i)
            {
                u32 t0 = b0->typeIn.data[i];
                u32 t1 = b1->typeIn.data[i];
                u32 vt;
                bool m = EXP_evalTypeUnify(t0, t1, &vt);
                if (!m)
                {
                    EXP_evalVerifErrorAtNode(ctx, curCall->srcNode, EXP_EvalErrCode_EvalBranchUneq);
                    goto next;
                }
                assert(b1->typeIn.data[i] == curBlock->typeIn.data[i]);
                b0->typeIn.data[i] = vt;
                b1->typeIn.data[i] = vt;
                curBlock->typeIn.data[i] = vt;
            }
            // todo
            for (u32 i = 0; i < b0->typeOut.length; ++i)
            {
                u32 t0 = b0->typeOut.data[i].id;
                u32 t1 = b1->typeOut.data[i].id;
                u32 vt;
                bool m = EXP_evalTypeUnify(t0, t1, &vt);
                if (!m)
                {
                    EXP_evalVerifErrorAtNode(ctx, curCall->srcNode, EXP_EvalErrCode_EvalBranchUneq);
                    goto next;
                }
                assert(b1->typeOut.data[i].id == curBlock->typeOut.data[i].id);
                b0->typeOut.data[i].id = vt;
                b1->typeOut.data[i].id = vt;
                curBlock->typeOut.data[i].id = vt;
            }
            goto next;
        }
        default:
            assert(false);
            return;
        }
        return;
    }
    EXP_Node node = *(curCall->p++);
    EXP_evalVerifNode(ctx, node, curCall, curBlock);
    goto next;
}








static void EXP_evalVerifRecheck(EXP_EvalVerifContext* ctx)
{
    EXP_Space* space = ctx->space;
    ctx->dataStackShiftEnable = true;
    ctx->recheckFlag = true;
    for (u32 i = 0; i < ctx->recheckNodes.length; ++i)
    {
        ctx->dataStack.length = 0;
        EXP_Node* pNode = ctx->recheckNodes.data + i;
        EXP_Node parent = EXP_evalVerifGetBlock(ctx, *pNode)->parent;
        EXP_EvalVerifCall blk = { parent, 0, pNode, pNode + 1, EXP_EvalBlockCallback_NONE };
        vec_push(&ctx->callStack, blk);
        EXP_evalVerifCall(ctx);
        if (ctx->error.code)
        {
            return;
        }
    }
}









EXP_EvalError EXP_evalVerif
(
    EXP_Space* space, EXP_Node root,
    EXP_EvalValueTypeInfoTable* valueTypeTable, EXP_EvalNativeFunInfoTable* nativeFunTable,
    EXP_EvalNodeTable* nodeTable, vec_u32* typeStack, EXP_SpaceSrcInfo* srcInfo
)
{
    EXP_EvalError error = { 0 };
    //return error;
    if (!EXP_isSeq(space, root))
    {
        return error;
    }
    EXP_EvalVerifContext _ctx = EXP_newEvalVerifContext(space, valueTypeTable, nativeFunTable, nodeTable, srcInfo);
    EXP_EvalVerifContext* ctx = &_ctx;

    vec_dup(&ctx->dataStack, typeStack);

    u32 len = EXP_seqLen(space, root);
    EXP_Node* seq = EXP_seqElm(space, root);
    EXP_evalVerifEnterBlock(ctx, seq, len, root, EXP_Node_Invalid, EXP_EvalBlockCallback_NONE, true);
    if (ctx->error.code)
    {
        error = ctx->error;
        EXP_evalVerifContextFree(ctx);
        return error;
    }
    EXP_evalVerifCall(ctx);
    if (!ctx->error.code)
    {
        vec_dup(typeStack, &ctx->dataStack);
    }
    if (!ctx->error.code)
    {
        EXP_evalVerifRecheck(ctx);
    }
    if (!ctx->error.code)
    {
        for (u32 i = 0; i < ctx->blockTable.length; ++i)
        {
            EXP_EvalVerifBlock* vb = ctx->blockTable.data + i;
            EXP_EvalNode* enode = nodeTable->data + i + ctx->blockTableBase;
            for (u32 i = 0; i < vb->defs.length; ++i)
            {
                EXP_EvalVerifDef* vdef = vb->defs.data + i;
                if (vdef->isVar)
                {
                    ++enode->varsCount;
                }
            }
            assert(enode->varsCount == vb->varsCount);
        }
    }
    error = ctx->error;
    EXP_evalVerifContextFree(ctx);
    return error;
}

















































































































































