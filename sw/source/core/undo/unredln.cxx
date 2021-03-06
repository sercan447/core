/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <UndoRedline.hxx>
#include <hintids.hxx>
#include <unotools/charclass.hxx>
#include <doc.hxx>
#include <IDocumentRedlineAccess.hxx>
#include <swundo.hxx>
#include <pam.hxx>
#include <ndtxt.hxx>
#include <txtfrm.hxx>
#include <UndoCore.hxx>
#include <UndoDelete.hxx>
#include <strings.hrc>
#include <redline.hxx>
#include <docary.hxx>
#include <sortopt.hxx>
#include <docedt.hxx>

SwUndoRedline::SwUndoRedline( SwUndoId nUsrId, const SwPaM& rRange )
    : SwUndo( SwUndoId::REDLINE, rRange.GetDoc() ), SwUndRng( rRange ),
    mnUserId( nUsrId ),
    mbHiddenRedlines( false )
{
    // consider Redline
    SwDoc& rDoc = *rRange.GetDoc();
    if( rDoc.getIDocumentRedlineAccess().IsRedlineOn() )
    {
        switch( mnUserId )
        {
        case SwUndoId::DELETE:
        case SwUndoId::REPLACE:
            mpRedlData.reset( new SwRedlineData( RedlineType::Delete, rDoc.getIDocumentRedlineAccess().GetRedlineAuthor() ) );
            break;
        default:
            ;
        }
        SetRedlineFlags( rDoc.getIDocumentRedlineAccess().GetRedlineFlags() );
    }

    sal_uLong nEndExtra = rDoc.GetNodes().GetEndOfExtras().GetIndex();

    mpRedlSaveData.reset( new SwRedlineSaveDatas );
    if( !FillSaveData( rRange, *mpRedlSaveData, false, SwUndoId::REJECT_REDLINE != mnUserId ))
    {
        mpRedlSaveData.reset();
    }
    else
    {
        mbHiddenRedlines = HasHiddenRedlines( *mpRedlSaveData );
        if( mbHiddenRedlines )           // then the NodeIndices of SwUndRng need to be corrected
        {
            nEndExtra -= rDoc.GetNodes().GetEndOfExtras().GetIndex();
            m_nSttNode -= nEndExtra;
            m_nEndNode -= nEndExtra;
        }
    }
}

SwUndoRedline::~SwUndoRedline()
{
    mpRedlData.reset();
    mpRedlSaveData.reset();
}

sal_uInt16 SwUndoRedline::GetRedlSaveCount() const
{
    return mpRedlSaveData ? mpRedlSaveData->size() : 0;
}

void SwUndoRedline::UndoImpl(::sw::UndoRedoContext & rContext)
{
    SwDoc& rDoc = rContext.GetDoc();
    SwPaM& rPam(AddUndoRedoPaM(rContext));

    UndoRedlineImpl(rDoc, rPam);

    if( mpRedlSaveData )
    {
        sal_uLong nEndExtra = rDoc.GetNodes().GetEndOfExtras().GetIndex();
        SetSaveData(rDoc, *mpRedlSaveData);
        if( mbHiddenRedlines )
        {
            mpRedlSaveData->clear();

            nEndExtra = rDoc.GetNodes().GetEndOfExtras().GetIndex() - nEndExtra;
            m_nSttNode += nEndExtra;
            m_nEndNode += nEndExtra;
        }
        SetPaM(rPam, true);
    }

    // update frames after calling SetSaveData
    if (dynamic_cast<SwUndoRedlineDelete*>(this))
    {
        sw::UpdateFramesForRemoveDeleteRedline(rDoc, rPam);
    }
    else if (dynamic_cast<SwUndoAcceptRedline*>(this)
          || dynamic_cast<SwUndoRejectRedline*>(this))
    {   // (can't check here if there's a delete redline being accepted)
        sw::UpdateFramesForAddDeleteRedline(rDoc, rPam);
    }
}

void SwUndoRedline::RedoImpl(::sw::UndoRedoContext & rContext)
{
    SwDoc& rDoc = rContext.GetDoc();
    RedlineFlags eOld = rDoc.getIDocumentRedlineAccess().GetRedlineFlags();
    rDoc.getIDocumentRedlineAccess().SetRedlineFlags_intern(( eOld & ~RedlineFlags::Ignore) | RedlineFlags::On );

    SwPaM & rPam( AddUndoRedoPaM(rContext) );
    if( mpRedlSaveData && mbHiddenRedlines )
    {
        sal_uLong nEndExtra = rDoc.GetNodes().GetEndOfExtras().GetIndex();
        FillSaveData(rPam, *mpRedlSaveData, false, SwUndoId::REJECT_REDLINE != mnUserId );

        nEndExtra -= rDoc.GetNodes().GetEndOfExtras().GetIndex();
        m_nSttNode -= nEndExtra;
        m_nEndNode -= nEndExtra;
    }

    RedoRedlineImpl(rDoc, rPam);

    SetPaM(rPam, true);
    rDoc.getIDocumentRedlineAccess().SetRedlineFlags_intern( eOld );
}

void SwUndoRedline::UndoRedlineImpl(SwDoc &, SwPaM &)
{
}

// default: remove redlines
void SwUndoRedline::RedoRedlineImpl(SwDoc & rDoc, SwPaM & rPam)
{
    rDoc.getIDocumentRedlineAccess().DeleteRedline(rPam, true, RedlineType::Any);
}

SwUndoRedlineDelete::SwUndoRedlineDelete( const SwPaM& rRange, SwUndoId nUsrId )
    : SwUndoRedline( nUsrId != SwUndoId::EMPTY ? nUsrId : SwUndoId::DELETE, rRange ),
    bCanGroup( false ), bIsDelim( false ), bIsBackspace( false )
{
    const SwTextNode* pTNd;
    SetRedlineText(rRange.GetText());
    if( SwUndoId::DELETE == mnUserId &&
        m_nSttNode == m_nEndNode && m_nSttContent + 1 == m_nEndContent &&
        nullptr != (pTNd = rRange.GetNode().GetTextNode()) )
    {
        sal_Unicode const cCh = pTNd->GetText()[m_nSttContent];
        if( CH_TXTATR_BREAKWORD != cCh && CH_TXTATR_INWORD != cCh )
        {
            bCanGroup = true;
            bIsDelim = !GetAppCharClass().isLetterNumeric( pTNd->GetText(),
                                                            m_nSttContent );
            bIsBackspace = m_nSttContent == rRange.GetPoint()->nContent.GetIndex();
        }
    }

    m_bCacheComment = false;
}

// bit of a hack, replace everything...
SwRewriter SwUndoRedlineDelete::GetRewriter() const
{
    SwRewriter aResult;
    OUString aStr = DenoteSpecialCharacters(m_sRedlineText);
    aStr = ShortenString(aStr, nUndoStringLength, SwResId(STR_LDOTS));
    SwRewriter aRewriter;
    aRewriter.AddRule(UndoArg1, aStr);
    OUString ret = aRewriter.Apply(SwResId(STR_UNDO_REDLINE_DELETE));
    aResult.AddRule(UndoArg1, ret);
    return aResult;
}

void SwUndoRedlineDelete::SetRedlineText(const OUString & rText)
{
    m_sRedlineText = rText;
}

void SwUndoRedlineDelete::UndoRedlineImpl(SwDoc & rDoc, SwPaM & rPam)
{
    rDoc.getIDocumentRedlineAccess().DeleteRedline(rPam, true, RedlineType::Any);
}

void SwUndoRedlineDelete::RedoRedlineImpl(SwDoc & rDoc, SwPaM & rPam)
{
    if (rPam.GetPoint() != rPam.GetMark())
    {
        rDoc.getIDocumentRedlineAccess().AppendRedline( new SwRangeRedline(*mpRedlData, rPam), false );
    }
    sw::UpdateFramesForAddDeleteRedline(rDoc, rPam);
}

bool SwUndoRedlineDelete::CanGrouping( const SwUndoRedlineDelete& rNext )
{
    bool bRet = false;
    if( SwUndoId::DELETE == mnUserId && mnUserId == rNext.mnUserId &&
        bCanGroup == rNext.bCanGroup &&
        bIsDelim == rNext.bIsDelim &&
        bIsBackspace == rNext.bIsBackspace &&
        m_nSttNode == m_nEndNode &&
        rNext.m_nSttNode == m_nSttNode &&
        rNext.m_nEndNode == m_nEndNode )
    {
        int bIsEnd = 0;
        if( rNext.m_nSttContent == m_nEndContent )
            bIsEnd = 1;
        else if( rNext.m_nEndContent == m_nSttContent )
            bIsEnd = -1;

        if( bIsEnd &&
            (( !mpRedlSaveData && !rNext.mpRedlSaveData ) ||
             ( mpRedlSaveData && rNext.mpRedlSaveData &&
                SwUndo::CanRedlineGroup( *mpRedlSaveData,
                            *rNext.mpRedlSaveData, 1 != bIsEnd )
             )))
        {
            if( 1 == bIsEnd )
                m_nEndContent = rNext.m_nEndContent;
            else
                m_nSttContent = rNext.m_nSttContent;
            bRet = true;
        }
    }
    return bRet;
}

SwUndoRedlineSort::SwUndoRedlineSort( const SwPaM& rRange,
                                    const SwSortOptions& rOpt )
    : SwUndoRedline( SwUndoId::SORT_TXT, rRange ),
    pOpt( new SwSortOptions( rOpt ) ),
    nSaveEndNode( m_nEndNode ), nSaveEndContent( m_nEndContent )
{
}

SwUndoRedlineSort::~SwUndoRedlineSort()
{
}

void SwUndoRedlineSort::UndoRedlineImpl(SwDoc & rDoc, SwPaM & rPam)
{
    // rPam contains the sorted range
    // aSaveRange contains copied (i.e. original) range

    SwPosition *const pStart = rPam.Start();
    SwPosition *const pEnd   = rPam.End();

    SwNodeIndex aPrevIdx( pStart->nNode, -1 );
    sal_uLong nOffsetTemp = pEnd->nNode.GetIndex() - pStart->nNode.GetIndex();

    if( !( RedlineFlags::ShowDelete & rDoc.getIDocumentRedlineAccess().GetRedlineFlags()) )
    {
        // Search both Redline objects and make them visible to make the nodes
        // consistent again. The 'delete' one is hidden, thus search for the
        // 'insert' Redline object. The former is located directly after the latter.
        SwRedlineTable::size_type nFnd = rDoc.getIDocumentRedlineAccess().GetRedlinePos(
                            *rDoc.GetNodes()[ m_nSttNode + 1 ],
                            RedlineType::Insert );
        OSL_ENSURE( SwRedlineTable::npos != nFnd && nFnd+1 < rDoc.getIDocumentRedlineAccess().GetRedlineTable().size(),
                    "could not find an Insert object" );
        ++nFnd;
        rDoc.getIDocumentRedlineAccess().GetRedlineTable()[nFnd]->Show(1, nFnd);
    }

    {
        SwPaM aTmp( *rPam.GetMark() );
        aTmp.GetMark()->nContent = 0;
        aTmp.SetMark();
        aTmp.GetPoint()->nNode = nSaveEndNode;
        aTmp.GetPoint()->nContent.Assign( aTmp.GetContentNode(), nSaveEndContent );
        rDoc.getIDocumentRedlineAccess().DeleteRedline( aTmp, true, RedlineType::Any );
    }

    rDoc.getIDocumentContentOperations().DelFullPara(rPam);

    SwPaM *const pPam = & rPam;
    pPam->DeleteMark();
    pPam->GetPoint()->nNode.Assign( aPrevIdx.GetNode(), +1 );
    SwContentNode* pCNd = pPam->GetContentNode();
    pPam->GetPoint()->nContent.Assign(pCNd, 0 );
    pPam->SetMark();

    pPam->GetPoint()->nNode += nOffsetTemp;
    pCNd = pPam->GetContentNode();
    pPam->GetPoint()->nContent.Assign( pCNd, pCNd->Len() );

    SetValues( *pPam );

    SetPaM(rPam);
}

void SwUndoRedlineSort::RedoRedlineImpl(SwDoc & rDoc, SwPaM & rPam)
{
    SwPaM* pPam = &rPam;
    SwPosition* pStart = pPam->Start();
    SwPosition* pEnd   = pPam->End();

    SwNodeIndex aPrevIdx( pStart->nNode, -1 );
    sal_uLong nOffsetTemp = pEnd->nNode.GetIndex() - pStart->nNode.GetIndex();
    const sal_Int32 nCntStt  = pStart->nContent.GetIndex();

    rDoc.SortText(rPam, *pOpt);

    pPam->DeleteMark();
    pPam->GetPoint()->nNode.Assign( aPrevIdx.GetNode(), +1 );
    SwContentNode* pCNd = pPam->GetContentNode();
    sal_Int32 nLen = pCNd->Len();
    if( nLen > nCntStt )
        nLen = nCntStt;
    pPam->GetPoint()->nContent.Assign(pCNd, nLen );
    pPam->SetMark();

    pPam->GetPoint()->nNode += nOffsetTemp;
    pCNd = pPam->GetContentNode();
    pPam->GetPoint()->nContent.Assign( pCNd, pCNd->Len() );

    SetValues( rPam );

    SetPaM( rPam );
    rPam.GetPoint()->nNode = nSaveEndNode;
    rPam.GetPoint()->nContent.Assign( rPam.GetContentNode(), nSaveEndContent );
}

void SwUndoRedlineSort::RepeatImpl(::sw::RepeatContext & rContext)
{
    rContext.GetDoc().SortText( rContext.GetRepeatPaM(), *pOpt );
}

void SwUndoRedlineSort::SetSaveRange( const SwPaM& rRange )
{
    const SwPosition& rPos = *rRange.End();
    nSaveEndNode = rPos.nNode.GetIndex();
    nSaveEndContent = rPos.nContent.GetIndex();
}

SwUndoAcceptRedline::SwUndoAcceptRedline( const SwPaM& rRange )
    : SwUndoRedline( SwUndoId::ACCEPT_REDLINE, rRange )
{
}

void SwUndoAcceptRedline::RedoRedlineImpl(SwDoc & rDoc, SwPaM & rPam)
{
    rDoc.getIDocumentRedlineAccess().AcceptRedline(rPam, false);
}

void SwUndoAcceptRedline::RepeatImpl(::sw::RepeatContext & rContext)
{
    rContext.GetDoc().getIDocumentRedlineAccess().AcceptRedline(rContext.GetRepeatPaM(), true);
}

SwUndoRejectRedline::SwUndoRejectRedline( const SwPaM& rRange )
    : SwUndoRedline( SwUndoId::REJECT_REDLINE, rRange )
{
}

void SwUndoRejectRedline::RedoRedlineImpl(SwDoc & rDoc, SwPaM & rPam)
{
    rDoc.getIDocumentRedlineAccess().RejectRedline(rPam, false);
}

void SwUndoRejectRedline::RepeatImpl(::sw::RepeatContext & rContext)
{
    rContext.GetDoc().getIDocumentRedlineAccess().RejectRedline(rContext.GetRepeatPaM(), true);
}

SwUndoCompDoc::SwUndoCompDoc( const SwPaM& rRg, bool bIns )
    : SwUndo( SwUndoId::COMPAREDOC, rRg.GetDoc() ), SwUndRng( rRg ),
    bInsert( bIns )
{
    SwDoc* pDoc = rRg.GetDoc();
    if( pDoc->getIDocumentRedlineAccess().IsRedlineOn() )
    {
        RedlineType eTyp = bInsert ? RedlineType::Insert : RedlineType::Delete;
        pRedlData.reset( new SwRedlineData( eTyp, pDoc->getIDocumentRedlineAccess().GetRedlineAuthor() ) );
        SetRedlineFlags( pDoc->getIDocumentRedlineAccess().GetRedlineFlags() );
    }
}

SwUndoCompDoc::SwUndoCompDoc( const SwRangeRedline& rRedl )
    : SwUndo( SwUndoId::COMPAREDOC, rRedl.GetDoc() ), SwUndRng( rRedl ),
    // for MergeDoc the corresponding inverse is needed
    bInsert( RedlineType::Delete == rRedl.GetType() )
{
    SwDoc* pDoc = rRedl.GetDoc();
    if( pDoc->getIDocumentRedlineAccess().IsRedlineOn() )
    {
        pRedlData.reset( new SwRedlineData( rRedl.GetRedlineData() ) );
        SetRedlineFlags( pDoc->getIDocumentRedlineAccess().GetRedlineFlags() );
    }

    pRedlSaveData.reset( new SwRedlineSaveDatas );
    if( !FillSaveData( rRedl, *pRedlSaveData, false ))
    {
        pRedlSaveData.reset();
    }
}

SwUndoCompDoc::~SwUndoCompDoc()
{
    pRedlData.reset();
    pUnDel.reset();
    pUnDel2.reset();
    pRedlSaveData.reset();
}

void SwUndoCompDoc::UndoImpl(::sw::UndoRedoContext & rContext)
{
    SwDoc& rDoc = rContext.GetDoc();
    SwPaM& rPam(AddUndoRedoPaM(rContext));

    if( !bInsert )
    {
        // delete Redlines
        RedlineFlags eOld = rDoc.getIDocumentRedlineAccess().GetRedlineFlags();
        rDoc.getIDocumentRedlineAccess().SetRedlineFlags_intern(( eOld & ~RedlineFlags::Ignore) | RedlineFlags::On);

        rDoc.getIDocumentRedlineAccess().DeleteRedline(rPam, true, RedlineType::Any);

        rDoc.getIDocumentRedlineAccess().SetRedlineFlags_intern( eOld );

        // per definition Point is end (in SwUndRng!)
        SwContentNode* pCSttNd = rPam.GetContentNode(false);
        SwContentNode* pCEndNd = rPam.GetContentNode();

        // if start- and end-content is zero, then the doc-compare moves
        // complete nodes into the current doc. And then the selection
        // must be from end to start, so the delete join into the right
        // direction.
        if( !m_nSttContent && !m_nEndContent )
            rPam.Exchange();

        bool bJoinText, bJoinPrev;
        sw_GetJoinFlags(rPam, bJoinText, bJoinPrev);

        pUnDel.reset( new SwUndoDelete(rPam, false) );

        if( bJoinText )
            sw_JoinText(rPam, bJoinPrev);

        if( pCSttNd && !pCEndNd)
        {
            // #112139# Do not step behind the end of content.
            SwNode & rTmp = rPam.GetNode();
            SwNode * pEnd = rDoc.GetNodes().DocumentSectionEndNode(&rTmp);

            if (&rTmp != pEnd)
            {
                rPam.SetMark();
                ++rPam.GetPoint()->nNode;
                rPam.GetBound().nContent.Assign( nullptr, 0 );
                rPam.GetBound( false ).nContent.Assign( nullptr, 0 );
                pUnDel2.reset( new SwUndoDelete(rPam, true) );
            }
        }
        rPam.DeleteMark();
    }
    else
    {
        if( IDocumentRedlineAccess::IsRedlineOn( GetRedlineFlags() ))
        {
            rDoc.getIDocumentRedlineAccess().DeleteRedline(rPam, true, RedlineType::Any);

            if( pRedlSaveData )
                SetSaveData(rDoc, *pRedlSaveData);
        }
        SetPaM(rPam, true);
    }
}

void SwUndoCompDoc::RedoImpl(::sw::UndoRedoContext & rContext)
{
    SwDoc& rDoc = rContext.GetDoc();

    if( bInsert )
    {
        SwPaM& rPam(AddUndoRedoPaM(rContext));
        if( pRedlData && IDocumentRedlineAccess::IsRedlineOn( GetRedlineFlags() ))
        {
            SwRangeRedline* pTmp = new SwRangeRedline(*pRedlData, rPam);
            rDoc.getIDocumentRedlineAccess().GetRedlineTable().Insert( pTmp );
            pTmp->InvalidateRange(SwRangeRedline::Invalidation::Add);
        }
        else if( !( RedlineFlags::Ignore & GetRedlineFlags() ) &&
                !rDoc.getIDocumentRedlineAccess().GetRedlineTable().empty() )
        {
            rDoc.getIDocumentRedlineAccess().SplitRedline(rPam);
        }
        SetPaM(rPam, true);
    }
    else
    {
        if( pUnDel2 )
        {
            pUnDel2->UndoImpl(rContext);
            pUnDel2.reset();
        }
        pUnDel->UndoImpl(rContext);
        pUnDel.reset();

        // note: don't call SetPaM before executing Undo of members
        SwPaM& rPam(AddUndoRedoPaM(rContext));

        SwRangeRedline* pTmp = new SwRangeRedline(*pRedlData, rPam);
        rDoc.getIDocumentRedlineAccess().GetRedlineTable().Insert( pTmp );
        pTmp->InvalidateRange(SwRangeRedline::Invalidation::Add);

        SetPaM(rPam, true);
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
