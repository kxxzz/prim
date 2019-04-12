#include "exp_eval_a.h"
#include "exp_eval_type_a.h"




typedef enum EXP_EvalBlockCallbackType
{
    EXP_EvalBlockCallbackType_NONE,
    EXP_EvalBlockCallbackType_Ncall,
    EXP_EvalBlockCallbackType_Call,
    EXP_EvalBlockCallbackType_Cond,
} EXP_EvalBlockCallbackType;

typedef struct EXP_EvalBlockCallback
{
    EXP_EvalBlockCallbackType type;
    union
    {
        u32 afun;
        EXP_Node fun;
    };
} EXP_EvalBlockCallback;





typedef struct EXP_EvalCall
{
    EXP_Node srcNode;
    EXP_Node* p;
    EXP_Node* end;
    EXP_EvalBlockCallback cb;
} EXP_EvalCall;

typedef vec_t(EXP_EvalCall) EXP_EvalCallStack;



typedef struct EXP_EvalVar
{
    u32 type;
    EXP_EvalValue val;
} EXP_EvalVar;

typedef vec_t(EXP_EvalVar) EXP_EvalVarStack;



typedef struct EXP_EvalContext
{
    EXP_Space* space;
    EXP_SpaceSrcInfo srcInfo;
    EXP_EvalAtypeInfoVec atypeTable;
    EXP_EvalAfunInfoVec afunTable;
    EXP_EvalTypeContext* typeContext;
    EXP_EvalNodeTable nodeTable;

    vec_u32 typeStack;
    EXP_EvalCallStack callStack;
    EXP_EvalVarStack varStack;
    EXP_EvalValueVec dataStack;

    EXP_EvalError error;
    EXP_EvalValue ncallOutBuf[EXP_EvalAfunOuts_MAX];

    EXP_EvalFileInfoTable fileInfoTable;
} EXP_EvalContext;






EXP_EvalContext* EXP_newEvalContext(const EXP_EvalAtomTable* addAtomTable)
{
    EXP_EvalContext* ctx = zalloc(sizeof(*ctx));
    ctx->space = EXP_newSpace();
    for (u32 i = 0; i < EXP_NumEvalPrimTypes; ++i)
    {
        vec_push(&ctx->atypeTable, EXP_EvalPrimTypeInfoTable()[i]);
    }
    for (u32 i = 0; i < EXP_NumEvalPrimFuns; ++i)
    {
        vec_push(&ctx->afunTable, EXP_EvalPrimFunInfoTable()[i]);
    }
    if (addAtomTable)
    {
        for (u32 i = 0; i < addAtomTable->numTypes; ++i)
        {
            vec_push(&ctx->atypeTable, addAtomTable->types[i]);
        }
        for (u32 i = 0; i < addAtomTable->numFuns; ++i)
        {
            vec_push(&ctx->afunTable, addAtomTable->funs[i]);
        }
    }
    ctx->typeContext = EXP_newEvalTypeContext();
    return ctx;
}


static void EXP_evalValueDtor(EXP_EvalTypeContext* ctx, EXP_EvalAtypeInfoVec* atypeTable, u32 typeId, EXP_EvalValue val);


void EXP_evalContextFree(EXP_EvalContext* ctx)
{
    vec_free(&ctx->fileInfoTable);

    vec_u32* typeStack = EXP_evalDataTypeStack(ctx);
    EXP_EvalValueVec* dataStack = EXP_evalDataStack(ctx);
    for (u32 i = 0; i < dataStack->length; ++i)
    {
        u32 t = typeStack->data[i];
        EXP_EvalValue v = dataStack->data[i];
        EXP_evalValueDtor(ctx->typeContext, &ctx->atypeTable, t, v);
    }

    vec_free(&ctx->dataStack);
    vec_free(&ctx->varStack);
    vec_free(&ctx->callStack);
    vec_free(&ctx->typeStack);

    vec_free(&ctx->nodeTable);
    EXP_evalTypeContextFree(ctx->typeContext);
    vec_free(&ctx->afunTable);
    vec_free(&ctx->atypeTable);
    EXP_spaceSrcInfoFree(&ctx->srcInfo);
    EXP_spaceFree(ctx->space);
    free(ctx);
}












EXP_EvalError EXP_evalLastError(EXP_EvalContext* ctx)
{
    return ctx->error;
}

EXP_EvalTypeContext* EXP_evalDataTypeContext(EXP_EvalContext* ctx)
{
    return ctx->typeContext;
}

vec_u32* EXP_evalDataTypeStack(EXP_EvalContext* ctx)
{
    return &ctx->typeStack;
}

EXP_EvalValueVec* EXP_evalDataStack(EXP_EvalContext* ctx)
{
    return &ctx->dataStack;
}













static EXP_EvalValue EXP_evalValueCopier(EXP_EvalTypeContext* ctx, u32 typeId, EXP_EvalValue src)
{
    EXP_EvalValue dup = { 0 };
    return dup;
}


static void EXP_evalValueDtor(EXP_EvalTypeContext* ctx, EXP_EvalAtypeInfoVec* atypeTable, u32 typeId, EXP_EvalValue val)
{
    const EXP_EvalTypeDesc* desc = EXP_evalTypeDescById(ctx, typeId);
    switch (desc->type)
    {
    case EXP_EvalTypeType_Atom:
    {
        EXP_EvalAtypeInfo* atypeInfo = atypeTable->data + desc->atom;
        if (atypeInfo->dtor)
        {
            atypeInfo->dtor(val);
        }
        break;
    }
    default:
        // todo
        assert(false);
        break;
    }
}














void EXP_evalPushValue(EXP_EvalContext* ctx, u32 type, EXP_EvalValue val)
{
    vec_push(&ctx->typeStack, type);
    EXP_EvalValue val1 = EXP_evalValueCopier(ctx->typeContext, type, val);
    vec_push(&ctx->dataStack, val1);
}

void EXP_evalDrop(EXP_EvalContext* ctx)
{
    assert(ctx->typeStack.length > 0);
    assert(ctx->dataStack.length > 0);
    assert(ctx->typeStack.length == ctx->dataStack.length);
    u32 t = vec_last(&ctx->typeStack);
    EXP_EvalValue v = vec_last(&ctx->dataStack);
    EXP_evalValueDtor(ctx->typeContext, &ctx->atypeTable, t, v);
    vec_pop(&ctx->typeStack);
    vec_pop(&ctx->dataStack);
}













static void EXP_evalDefGetBody(EXP_EvalContext* ctx, EXP_Node node, u32* pLen, EXP_Node** pSeq)
{
    EXP_Space* space = ctx->space;
    assert(EXP_seqLen(space, node) >= 2);
    *pLen = EXP_seqLen(space, node) - 2;
    EXP_Node* defCall = EXP_seqElm(space, node);
    *pSeq = defCall + 2;
}













static EXP_EvalVar* EXP_evalGetVar(EXP_EvalContext* ctx, const EXP_EvalNodeVar* evar)
{
    EXP_Space* space = ctx->space;
    EXP_EvalNodeTable* nodeTable = &ctx->nodeTable;
    EXP_EvalCallStack* callStack = &ctx->callStack;
    EXP_EvalVarStack* varStack = &ctx->varStack;
    u32 offset = varStack->length;
    for (u32 i = 0; i < callStack->length; ++i)
    {
        EXP_EvalCall* call = callStack->data + callStack->length - 1 - i;
        EXP_EvalNode* blkEnode = nodeTable->data + call->srcNode.id;
        assert(offset >= blkEnode->varsCount);
        offset -= blkEnode->varsCount;
        if (evar->block.id == call->srcNode.id)
        {
            offset += evar->id;
            return &varStack->data[offset];
        }
    }
    return NULL;
}













static void EXP_evalEnterBlock(EXP_EvalContext* ctx, u32 len, EXP_Node* seq, EXP_Node srcNode)
{
    EXP_EvalBlockCallback nocb = { EXP_EvalBlockCallbackType_NONE };
    EXP_EvalCall call = { srcNode, seq, seq + len, nocb };
    vec_push(&ctx->callStack, call);
}

static void EXP_evalEnterBlockWithCB
(
    EXP_EvalContext* ctx, u32 len, EXP_Node* seq, EXP_Node srcNode, EXP_EvalBlockCallback cb
)
{
    assert(cb.type != EXP_EvalBlockCallbackType_NONE);
    EXP_EvalCall call = { srcNode, seq, seq + len, cb };
    vec_push(&ctx->callStack, call);
}

static bool EXP_evalLeaveBlock(EXP_EvalContext* ctx)
{
    EXP_EvalCall* curCall = &vec_last(&ctx->callStack);
    EXP_EvalNode* enode = ctx->nodeTable.data + curCall->srcNode.id;
    assert(ctx->varStack.length >= enode->varsCount);
    u32 varStackLength1 = ctx->varStack.length - enode->varsCount;
    for (u32 i = varStackLength1; i < ctx->varStack.length; ++i)
    {
        EXP_EvalVar* var = ctx->varStack.data + i;
        u32 t = var->type;
        EXP_EvalValue v = var->val;
        EXP_evalValueDtor(ctx->typeContext, &ctx->atypeTable, t, v);
    }
    vec_resize(&ctx->varStack, varStackLength1);
    vec_pop(&ctx->callStack);
    return ctx->callStack.length > 0;
}





static bool EXP_evalAtTail(EXP_EvalContext* ctx)
{
    EXP_EvalCall* curCall = &vec_last(&ctx->callStack);
    if (curCall->cb.type != EXP_EvalBlockCallbackType_NONE)
    {
        return false;
    }
    return curCall->p == curCall->end;
}







static void EXP_evalAfunCall
(
    EXP_EvalContext* ctx, EXP_EvalAfunInfo* afunInfo, EXP_Node srcNode
)
{
    EXP_Space* space = ctx->space;
    EXP_EvalValueVec* dataStack = &ctx->dataStack;
    EXP_EvalTypeContext* typeContext = ctx->typeContext;

    u32 argsOffset = dataStack->length - afunInfo->numIns;
    afunInfo->call(space, dataStack->data + argsOffset, ctx->ncallOutBuf);
    for (u32 i = 0; i < afunInfo->numIns; ++i)
    {
        u32 t = EXP_evalTypeAtom(typeContext, afunInfo->inAtype[i]);
        EXP_EvalValue v = dataStack->data[argsOffset + i];
        EXP_evalValueDtor(typeContext, &ctx->atypeTable, t, v);
    }
    vec_resize(dataStack, argsOffset);
    for (u32 i = 0; i < afunInfo->numOuts; ++i)
    {
        EXP_EvalValue v = ctx->ncallOutBuf[i];
        vec_push(dataStack, v);
    }
}








static void EXP_evalCall(EXP_EvalContext* ctx)
{
    EXP_Space* space = ctx->space;
    EXP_EvalCall* curCall;
    EXP_EvalNodeTable* nodeTable = &ctx->nodeTable;
    EXP_EvalValueVec* dataStack = &ctx->dataStack;
    EXP_EvalTypeContext* typeContext = ctx->typeContext;
next:
    if (ctx->error.code)
    {
        return;
    }
    curCall = &vec_last(&ctx->callStack);
    if (curCall->p == curCall->end)
    {
        EXP_EvalBlockCallback* cb = &curCall->cb;
        switch (cb->type)
        {
        case EXP_EvalBlockCallbackType_NONE:
        {
            break;
        }
        case EXP_EvalBlockCallbackType_Ncall:
        {
            EXP_EvalAfunInfo* afunInfo = ctx->afunTable.data + cb->afun;
            u32 numIns = afunInfo->numIns;
            EXP_evalAfunCall(ctx, afunInfo, curCall->srcNode);
            break;
        }
        case EXP_EvalBlockCallbackType_Call:
        {
            EXP_Node fun = cb->fun;
            u32 bodyLen = 0;
            EXP_Node* body = NULL;
            EXP_evalDefGetBody(ctx, fun, &bodyLen, &body);
            if (!EXP_evalLeaveBlock(ctx))
            {
                return;
            }
            // tail call optimization
            while (EXP_evalAtTail(ctx))
            {
                if (!EXP_evalLeaveBlock(ctx))
                {
                    break;
                }
            }
            EXP_evalEnterBlock(ctx, bodyLen, body, fun);
            goto next;
        }
        case EXP_EvalBlockCallbackType_Cond:
        {
            EXP_EvalValue v = vec_last(dataStack);
            vec_pop(dataStack);
            if (!EXP_evalLeaveBlock(ctx))
            {
                return;
            }
            EXP_Node srcNode = curCall->srcNode;
            if (v.b)
            {
                EXP_evalEnterBlock(ctx, 1, EXP_evalIfBranch0(space, srcNode), srcNode);
                goto next;
            }
            else if (EXP_evalIfHasBranch1(space, srcNode))
            {
                EXP_evalEnterBlock(ctx, 1, EXP_evalIfBranch1(space, srcNode), srcNode);
                goto next;
            }
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
    EXP_Node node = *(curCall->p++);
#ifndef NDEBUG
    EXP_NodeSrcInfo* nodeSrcInfo = ctx->srcInfo.nodes.data + node.id;
#endif
    EXP_EvalNode* enode = nodeTable->data + node.id;
    switch (enode->type)
    {
    case EXP_EvalNodeType_VarDefBegin:
    {
        assert(EXP_isTok(space, node));
        assert(EXP_EvalBlockCallbackType_NONE == curCall->cb.type);
        EXP_Node* begin = curCall->p;
        for (u32 n = 0;;)
        {
            assert(curCall->p < curCall->end);
            node = *(curCall->p++);
            assert(EXP_isTok(space, node));
            enode = nodeTable->data + node.id;
            if (EXP_EvalNodeType_VarDefEnd == enode->type)
            {
                assert(n <= dataStack->length);
                u32 off = dataStack->length - n;
                for (u32 i = 0; i < n; ++i)
                {
                    EXP_EvalValue val = dataStack->data[off + i];
                    EXP_EvalNode* eval = nodeTable->data + begin[0].id;
                    EXP_EvalValue val1 = EXP_evalValueCopier(typeContext, eval->valType, val);
                    EXP_EvalVar var = { eval->valType, val1 };
                    vec_push(&ctx->varStack, var);
                }
                for (u32 i = off; i < dataStack->length; ++i)
                {
                    EXP_EvalValue val = dataStack->data[off + i];
                    EXP_EvalNode* eval = nodeTable->data + begin[0].id;
                    EXP_evalValueDtor(typeContext, &ctx->atypeTable, eval->valType, val);
                }
                vec_resize(dataStack, off);
                goto next;
            }
            else
            {
                assert(EXP_EvalNodeType_VarDefCopy == enode->type);
            }
            ++n;
        }
    }
    case EXP_EvalNodeType_Drop:
    {
        assert(EXP_isTok(space, node));
        assert(dataStack->length > 0);
        u32 t = enode->valType;
        EXP_EvalValue v = vec_last(dataStack);
        EXP_evalValueDtor(typeContext, &ctx->atypeTable, t, v);
        vec_pop(dataStack);
        goto next;
    }
    case EXP_EvalNodeType_Afun:
    {
        assert(EXP_isTok(space, node));
        EXP_EvalAfunInfo* afunInfo = ctx->afunTable.data + enode->afun;
        assert(afunInfo->call);
        assert(dataStack->length >= afunInfo->numIns);
        EXP_evalAfunCall(ctx, afunInfo, node);
        goto next;
    }
    case EXP_EvalNodeType_Var:
    {
        assert(EXP_isTok(space, node));
        EXP_EvalVar* var = EXP_evalGetVar(ctx, &enode->var);
        EXP_EvalValue val1 = EXP_evalValueCopier(typeContext, var->type, var->val);
        vec_push(dataStack, val1);
        goto next;
    }
    case EXP_EvalNodeType_Fun:
    {
        assert(EXP_isTok(space, node));
        EXP_Node funDef = enode->funDef;
        u32 bodyLen = 0;
        EXP_Node* body = NULL;
        EXP_evalDefGetBody(ctx, funDef, &bodyLen, &body);
        EXP_evalEnterBlock(ctx, bodyLen, body, funDef);
        goto next;
    }
    case EXP_EvalNodeType_Value:
    {
        assert(EXP_isTok(space, node));
        u32 t = enode->valType;
        EXP_EvalValue v = enode->value;
        EXP_EvalValue v1 = EXP_evalValueCopier(typeContext, t, v);
        vec_push(dataStack, v1);
        goto next;
    }
    case EXP_EvalNodeType_String:
    {
        assert(EXP_isTok(space, node));
        EXP_EvalValue v = { 0 };
        u32 l = EXP_tokSize(space, node);
        const char* s = EXP_tokCstr(space, node);
        v.s = (vec_char*)zalloc(sizeof(vec_char));
        vec_pusharr(v.s, s, l);
        vec_push(v.s, 0);
        vec_push(dataStack, v);
        goto next;
    }
    case EXP_EvalNodeType_CallVar:
    {
        assert(EXP_evalCheckCall(space, node));
        // todo
        assert(false);
        goto next;
    }
    case EXP_EvalNodeType_CallFun:
    {
        assert(EXP_evalCheckCall(space, node));
        EXP_Node* elms = EXP_seqElm(space, node);
        u32 len = EXP_seqLen(space, node);
        EXP_EvalBlockCallback cb = { EXP_EvalBlockCallbackType_Call, .fun = enode->funDef };
        EXP_evalEnterBlockWithCB(ctx, len - 1, elms + 1, node, cb);
        goto next;
    }
    case EXP_EvalNodeType_CallAfun:
    {
        assert(EXP_evalCheckCall(space, node));
        EXP_Node* elms = EXP_seqElm(space, node);
        u32 len = EXP_seqLen(space, node);
        u32 afun = enode->afun;
        assert(afun != -1);
        EXP_EvalAfunInfo* afunInfo = ctx->afunTable.data + afun;
        assert(afunInfo->call);
        EXP_EvalBlockCallback cb = { EXP_EvalBlockCallbackType_Ncall, .afun = afun };
        EXP_evalEnterBlockWithCB(ctx, len - 1, elms + 1, node, cb);
        goto next;
    }
    case EXP_EvalNodeType_Def:
    {
        assert(EXP_evalCheckCall(space, node));
        assert(EXP_EvalBlockCallbackType_NONE == curCall->cb.type);
        goto next;
    }
    case EXP_EvalNodeType_If:
    {
        assert(EXP_evalCheckCall(space, node));
        EXP_Node* elms = EXP_seqElm(space, node);
        u32 len = EXP_seqLen(space, node);
        assert((3 == len) || (4 == len));
        EXP_EvalBlockCallback cb = { EXP_EvalBlockCallbackType_Cond };
        EXP_evalEnterBlockWithCB(ctx, 1, elms + 1, node, cb);
        goto next;
    }
    case EXP_EvalNodeType_Block:
    {
        assert(EXP_evalCheckCall(space, node));
        // todo
        goto next;
    }
    default:
        assert(false);
    }
}



















void EXP_evalBlock(EXP_EvalContext* ctx, EXP_Node root)
{
    EXP_Space* space = ctx->space;
    u32 len = EXP_seqLen(space, root);
    EXP_Node* seq = EXP_seqElm(space, root);
    EXP_evalEnterBlock(ctx, len, seq, root);
    EXP_evalCall(ctx);
}









EXP_EvalError EXP_evalCompile
(
    EXP_Space* space, EXP_Node root, EXP_SpaceSrcInfo* srcInfo,
    EXP_EvalAtypeInfoVec* atypeTable, EXP_EvalAfunInfoVec* afunTable,
    EXP_EvalNodeTable* nodeTable,
    EXP_EvalTypeContext* typeContext, vec_u32* typeStack
);



bool EXP_evalCode(EXP_EvalContext* ctx, const char* filename, const char* code, bool enableSrcInfo)
{
    if (enableSrcInfo)
    {
        EXP_EvalFileInfo fileInfo = { 0 };
        stzncpy(fileInfo.name, filename, EXP_EvalFileName_MAX);
        vec_push(&ctx->fileInfoTable, fileInfo);
    }
    EXP_SpaceSrcInfo* srcInfo = NULL;
    if (enableSrcInfo)
    {
        srcInfo = &ctx->srcInfo;
    }
    EXP_Space* space = ctx->space;
    EXP_Node root = EXP_loadSrcAsList(space, code, srcInfo);
    if (EXP_NodeId_Invalid == root.id)
    {
        ctx->error.code = EXP_EvalErrCode_ExpSyntax;
        ctx->error.file = 0;
        if (srcInfo)
        {
#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable : 6011)
#endif
            ctx->error.line = vec_last(&srcInfo->nodes).line;
            ctx->error.column = vec_last(&srcInfo->nodes).column;
#ifdef _MSC_VER
# pragma warning(pop)
#endif
        }
        else
        {
            ctx->error.line = -1;
            ctx->error.column = -1;
        }
        return false;
    }


    if (!EXP_isSeq(space, root))
    {
        ctx->error.code = EXP_EvalErrCode_EvalSyntax;
        ctx->error.file = srcInfo->nodes.data[root.id].file;
        ctx->error.line = srcInfo->nodes.data[root.id].line;
        ctx->error.column = srcInfo->nodes.data[root.id].column;
        return false;
    }
    EXP_EvalError error = EXP_evalCompile
    (
        space, root, &ctx->srcInfo,
        &ctx->atypeTable, &ctx->afunTable,
        &ctx->nodeTable,
        ctx->typeContext, &ctx->typeStack
    );
    if (error.code)
    {
        ctx->error = error;
        return false;
    }
    EXP_evalBlock(ctx, root);
    return true;
}



bool EXP_evalFile(EXP_EvalContext* ctx, const char* fileName, bool enableSrcInfo)
{
    char* code = NULL;
    u32 codeSize = FILEU_readFile(fileName, &code);
    if (-1 == codeSize)
    {
        ctx->error.code = EXP_EvalErrCode_SrcFile;
        ctx->error.file = 0;
        return false;
    }
    if (0 == codeSize)
    {
        return false;
    }
    bool r = EXP_evalCode(ctx, fileName, code, enableSrcInfo);
    free(code);
    return r;
}

























const EXP_EvalFileInfoTable* EXP_evalFileInfoTable(EXP_EvalContext* ctx)
{
    return &ctx->fileInfoTable;
}













const char** EXP_EvalErrCodeNameTable(void)
{
    static const char* a[EXP_NumEvalErrorCodes] =
    {
        "NONE",
        "SrcFile",
        "ExpSyntax",
        "EvalSyntax",
        "EvalUnkWord",
        "EvalUnkCall",
        "EvalArgs",
        "EvalBranchUneq",
        "EvalRecurNoBaseCase",
        "EvalUnification",
    };
    return a;
}












































































































