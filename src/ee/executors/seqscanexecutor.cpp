/* This file is part of VoltDB.
 * Copyright (C) 2008-2012 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
 *
 * VoltDB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VoltDB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "seqscanexecutor.h"
#include "common/debuglog.h"
#include "common/common.h"
#include "common/tabletuple.h"
#include "common/FatalException.hpp"
#include "expressions/abstractexpression.h"
#include "plannodes/seqscannode.h"
#include "plannodes/projectionnode.h"
#include "plannodes/limitnode.h"
#include "storage/persistenttable.h"
#include "storage/temptable.h"
#include "storage/tableiterator.h"

using namespace voltdb;

void SeqScanExecutor::p_setOutputTable(TempTableLimits* limits)
{
    assert(m_targetTable);
    SeqScanPlanNode* node = dynamic_cast<SeqScanPlanNode*>(m_abstractNode);
    assert(node);
    //
    // OPTIMIZATION: If there is no predicate for this SeqScan,
    // then we want to just set our OutputTable pointer to be the
    // pointer of our TargetTable. This prevents us from just
    // reading through the entire TargetTable and copying all of
    // the tuples. We are guarenteed that no Executor will ever
    // modify an input table, so this operation is safe
    //
    if (node->getPredicate() == NULL && node->getInlinePlanNodes().size() == 0)
    {
        m_outputTable = m_targetTable;
        return;
    }
    //
    // Otherwise create a new temp table that mirrors the
    // output schema specified in the plan (which should mirror
    // the output schema for any inlined projection)
    setTempOutputTable(limits, m_targetTable->name());
}

bool SeqScanExecutor::p_init()
{
    VOLT_TRACE("init SeqScan Executor");
    return true;
}

bool SeqScanExecutor::p_execute() {
    SeqScanPlanNode* node = dynamic_cast<SeqScanPlanNode*>(m_abstractNode);
    assert(node);
    assert(m_outputTable);

    assert(m_targetTable);
    //cout << "SeqScanExecutor: node id" << node->getPlanNodeId() << endl;
    VOLT_TRACE("Sequential Scanning table :\n %s",
               m_targetTable->debug().c_str());
    VOLT_DEBUG("Sequential Scanning table : %s which has %d active, %d"
               " allocated, %d used tuples",
               m_targetTable->name().c_str(),
               (int)m_targetTable->activeTupleCount(),
               (int)m_targetTable->allocatedTupleCount(),
               (int)m_targetTable->usedTupleCount());

    //
    // OPTIMIZATION: NESTED PROJECTION
    //
    int num_of_columns = (int)m_outputTable->columnCount();
    ProjectionPlanNode* projection_node = dynamic_cast<ProjectionPlanNode*>(node->getInlinePlanNode(PLAN_NODE_TYPE_PROJECTION));

    //
    // OPTIMIZATION: NESTED LIMIT
    // How nice! We can also cut off our scanning with a nested limit!
    //
    LimitPlanNode* limit_node = dynamic_cast<LimitPlanNode*>(node->getInlinePlanNode(PLAN_NODE_TYPE_LIMIT));

    //
    // OPTIMIZATION:
    //
    // If there is no predicate and no Projection for this SeqScan,
    // then we have already set the node's OutputTable to just point
    // at the TargetTable. Therefore, there is nothing we more we need
    // to do here
    //
    if (node->getPredicate() != NULL || projection_node != NULL ||
        limit_node != NULL)
    {
        //
        // Just walk through the table using our iterator and apply
        // the predicate to each tuple. For each tuple that satisfies
        // our expression, we'll insert them into the output table.
        //
        TableTuple tuple(m_targetTable->schema());
        TableIterator iterator = m_targetTable->iterator();
        AbstractExpression *predicate = node->getPredicate();
        VOLT_TRACE("SCAN PREDICATE A:\n%s\n", predicate->debug(true).c_str());

        if (predicate)
        {
            VOLT_DEBUG("SCAN PREDICATE B:\n%s\n",
                       predicate->debug(true).c_str());
        }

        int limit = -1;
        int offset = -1;
        if (limit_node) {
            limit_node->getLimitAndOffsetByReference(limit, offset);
        }

        TempTable* output_temp_table = dynamic_cast<TempTable*>(m_outputTable);

        int tuple_ctr = 0;
        int tuple_skipped = 0;
        while ((limit == -1 || tuple_ctr < limit) && iterator.next(tuple))
        {
            VOLT_TRACE("INPUT TUPLE: %s, %d/%d\n",
                       tuple.debug(m_targetTable->name()).c_str(), tuple_ctr,
                       (int)m_targetTable->activeTupleCount());
            //
            // For each tuple we need to evaluate it against our predicate
            //
            if (predicate == NULL || predicate->eval(&tuple, NULL).isTrue())
            {
                // Check if we have to skip this tuple because of offset
                if (tuple_skipped < offset) {
                    tuple_skipped++;
                    continue;
                }
                ++tuple_ctr;

                //
                // Nested Projection
                // Project (or replace) values from input tuple
                //
                if (projection_node != NULL)
                {
                    TableTuple &temp_tuple = m_outputTable->tempTuple();
                    for (int ctr = 0; ctr < num_of_columns; ctr++)
                    {
                        NValue value =
                            projection_node->
                          getOutputColumnExpressions()[ctr]->eval(&tuple, NULL);
                        temp_tuple.setNValue(ctr, value);
                    }
                    if ( ! output_temp_table->insertTempTuple(temp_tuple))
                    {
                        VOLT_ERROR("Failed to insert tuple from table '%s' into"
                                   " output table '%s'",
                                   m_targetTable->name().c_str(),
                                   m_outputTable->name().c_str());
                        return false;
                    }
                }
                else
                {
                    //
                    // Insert the tuple into our output table
                    //
                    if ( ! output_temp_table->insertTempTuple(tuple)) {
                        VOLT_ERROR("Failed to insert tuple from table '%s' into"
                                   " output table '%s'",
                                   m_targetTable->name().c_str(),
                                   m_outputTable->name().c_str());
                        return false;
                    }
                }
            }
        }
    }
    VOLT_TRACE("\n%s\n", m_outputTable->debug().c_str());
    VOLT_DEBUG("Finished Seq scanning");

    return true;
}
