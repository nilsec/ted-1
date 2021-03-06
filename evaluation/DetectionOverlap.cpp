#include <pipeline/Value.h>
#include <inference/LinearConstraints.h>
#include <inference/LinearObjective.h>
#include <inference/LinearSolver.h>
#include <util/Logger.h>
#include "DetectionOverlap.h"

logger::LogChannel detectionoverlaplog("detectionoverlaplog", "[DetectionOverlap] ");

DetectionOverlap::DetectionOverlap(bool headerOnly) :
		_headerOnly(headerOnly) {

	if (!_headerOnly) {

		registerInput(_stack1, "stack 1");
		registerInput(_stack2, "stack 2");
	}

	registerOutput(_errors, "errors");
}

void
DetectionOverlap::updateOutputs() {

	if (!_errors)
		_errors = new DetectionOverlapErrors();

	if (_headerOnly)
		return;

	if (_stack1->size() != 1 || _stack2->size() != 1)
		UTIL_THROW_EXCEPTION(
				UsageError,
				"The DetectionOverlap loss only accepts single 2D images");

	typedef std::pair<size_t, size_t> pair_t;

	std::map<size_t, util::point<float> > gtCenters;
	std::map<size_t, util::point<float> > recCenters;
	std::set<size_t> gtLabels;
	std::set<size_t> recLabels;
	std::map<size_t, unsigned int> gtSizes;
	std::map<size_t, unsigned int> recSizes;

	getCenterPoints(*(*_stack1)[0], gtCenters, gtLabels, gtSizes);
	getCenterPoints(*(*_stack2)[0], recCenters, recLabels, recSizes);

	LOG_DEBUG(detectionoverlaplog) << "there are " << gtCenters.size() << " ground truth regions" << std::endl;
	LOG_DEBUG(detectionoverlaplog) << "there are " << recCenters.size() << " reconstruction regions" << std::endl;

	std::set<pair_t>                  overlapPairs;
	std::map<pair_t, unsigned int>    overlapAreas;
	std::map<size_t, std::set<size_t> > gtToRecOverlaps;
	std::map<size_t, std::set<size_t> > recToGtOverlaps;

	getOverlaps(
			*(*_stack1)[0],
			*(*_stack2)[0],
			overlapPairs,
			overlapAreas,
			gtToRecOverlaps,
			recToGtOverlaps);

	LOG_DEBUG(detectionoverlaplog) << "ground truth contains "   << gtToRecOverlaps.size() << " regions with overlapping reconstruction regions" << std::endl;
	LOG_DEBUG(detectionoverlaplog) << "reconstruction contains " << recToGtOverlaps.size() << " regions with overlapping ground truth regions" << std::endl;
	LOG_DEBUG(detectionoverlaplog) << "found " << overlapPairs.size() << " possible matches by overlap" << std::endl;

	// get a score for each possible overlap
	std::map<pair_t, float> matchingScore;
	float maxScore = 0;
	foreach (const pair_t& p, overlapPairs) {

		util::point<float> gtCenter  = gtCenters[p.first];
		util::point<float> recCenter = recCenters[p.second];

		// ensure that the score is strictly positive (so that we break ties in 
		// the ILP later)
		float score =
				std::max(
						0.5,
						sqrt(
								pow(gtCenter.x - recCenter.x, 2.0) +
								pow(gtCenter.y - recCenter.y, 2.0)));

		matchingScore[p] = score;
		maxScore = std::max(score, maxScore);
	}

	// to select as many matches as possible but still minimize center distance, 
	// make score negative by subtracting largest distance
	foreach (const pair_t& p, overlapPairs)
		matchingScore[p] -= maxScore*1.1; // a little more to make every score negative

	// map pairs to variable numbers
	std::map<pair_t, unsigned int> pairToVariable;
	std::map<unsigned int, pair_t> variableToPair;

	unsigned int varNum = 0;
	foreach (const pair_t& p, overlapPairs) {

		pairToVariable[p] = varNum;
		variableToPair[varNum] = p;

		varNum++;
	}

	// find constraints:
	// • every region can map to at most one other

	// for each rec label
	//   for each gt label it overlaps with
	//     sum of pair indicators ≤ 1
	// (analogously for other direction)

	pipeline::Value<LinearConstraints> constraints;
	foreach (size_t recLabel, recLabels) {

		LinearConstraint constraint;
		foreach (size_t gtLabel, recToGtOverlaps[recLabel]) {

			unsigned int varNum = pairToVariable[std::make_pair(gtLabel, recLabel)];
			constraint.setCoefficient(varNum, 1.0);
		}

		constraint.setRelation(LessEqual);
		constraint.setValue(1.0);

		constraints->add(constraint);
	}

	foreach (size_t gtLabel, gtLabels) {

		LinearConstraint constraint;
		foreach (size_t recLabel, gtToRecOverlaps[gtLabel]) {

			unsigned int varNum = pairToVariable[std::make_pair(gtLabel, recLabel)];
			constraint.setCoefficient(varNum, 1.0);
		}

		constraint.setRelation(LessEqual);
		constraint.setValue(1.0);

		constraints->add(constraint);
	}

	// build objective
	pipeline::Value<LinearObjective> objective(overlapPairs.size());
	foreach (const pair_t& p, overlapPairs) {

		unsigned int varNum = pairToVariable[p];
		float        score  = matchingScore[p];

		objective->setCoefficient(varNum, score);
	}

	// solve

	pipeline::Process<LinearSolver> solver;
	pipeline::Value<LinearSolverParameters> parameters;
	parameters->setVariableType(Binary);

	solver->setInput("objective", objective);
	solver->setInput("linear constraints", constraints);
	solver->setInput("parameters", parameters);

	pipeline::Value<Solution> solution = solver->getOutput("solution");

	// get the optimal matching

	std::map<size_t, std::set<size_t> > gtToRecMatches;
	std::map<size_t, std::set<size_t> > recToGtMatches;
	std::set<pair_t> matches;
	for (unsigned int varNum = 0; varNum < overlapPairs.size(); varNum++) {

		LOG_ALL(detectionoverlaplog)
				<< "ILP solution for pair " << variableToPair[varNum].first
				<< ", " << variableToPair[varNum].second
				<< " = " << (*solution)[varNum] << std::endl;

		if ((*solution)[varNum] == 1) {

			pair_t match = variableToPair[varNum];

			gtToRecMatches[match.first].insert(match.second);
			recToGtMatches[match.second].insert(match.first);
			matches.insert(match);
		}
	}

	LOG_DEBUG(detectionoverlaplog)
			<< "found " << matches.size()
			<< " matches between ground truth and reconstruction"
			<< std::endl;

	// get FP and FN

	foreach (size_t gtLabel, gtLabels)
		if (gtToRecMatches.count(gtLabel) == 0)
			_errors->addFalseNegative(gtLabel);
	foreach (size_t recLabel, recLabels)
		if (recToGtMatches.count(recLabel) == 0)
			_errors->addFalsePositive(recLabel);

	// for each match, get area overlap measures
	foreach (const pair_t& p, matches) {

		// M1 = (R_rec ∩ R_gt)/(R_rec ∪ R_gt)*100
		// M2 = (R_rec ∩ R_gt)/R_gt*100

		// R_rec ∩ R_gt
		float cap = overlapAreas[p];
		// R_rec ∪ R_gt
		float cup = gtSizes[p.first] + recSizes[p.second] - overlapAreas[p];

		float m1 = (cap/cup)*100;
		float m2 = (cap/gtSizes[p.first])*100;
		float dice = 2.0*cap/(gtSizes[p.first] + recSizes[p.second]);

		LOG_ALL(detectionoverlaplog)
				<< "adding match with M1 = " << m1
				<< ", M2 = " << m2
				<< std::endl;

		_errors->addMatch(p, m1, m2, dice);
	}
}

void
DetectionOverlap::getCenterPoints(
		const Image&                          image,
		std::map<size_t, util::point<float> >& centers,
		std::set<size_t>&                      labels,
		std::map<size_t, unsigned int>&        sizes) {

	for (unsigned int y = 0; y < image.height(); y++)
		for (unsigned int x = 0; x < image.width(); x++) {

			size_t label = image(x, y);

			if (label == 0)
				continue;

			if (sizes.count(label) == 0) {

				sizes[label] = 1;
				centers[label] = util::point<float>(x, y);

			} else {

				sizes[label]++;
				centers[label] += util::point<float>(x, y);
			}

			labels.insert(label);
		}

	foreach (size_t label, labels)
		centers[label] /= sizes[label];
}

void
DetectionOverlap::getOverlaps(
		const Image& a,
		const Image& b,
		std::set<std::pair<size_t, size_t> >& overlapPairs,
		std::map<std::pair<size_t, size_t>, unsigned int>& overlapAreas,
		std::map<size_t, std::set<size_t> >& atob,
		std::map<size_t, std::set<size_t> >& btoa) {

	overlapPairs.clear();
	overlapAreas.clear();
	atob.clear();
	btoa.clear();

	for (unsigned int y = 0; y < a.height(); y++)
		for (unsigned int x = 0; x < a.width(); x++) {

			size_t labelA = a(x, y);
			size_t labelB = b(x, y);

			if (labelA == 0 || labelB == 0)
				continue;

			std::pair<size_t, size_t> p(labelA, labelB);
			overlapPairs.insert(p);
			if (overlapAreas.count(p))
				overlapAreas[p]++;
			else
				overlapAreas[p] = 1;
			atob[labelA].insert(labelB);
			btoa[labelB].insert(labelA);
		}
}
