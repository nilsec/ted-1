#include <boost/python/numeric.hpp>
#include <boost/python/dict.hpp>
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>

#include <util/Logger.h>
#include <util/ProgramOptions.h>
#include <util/helpers.hpp>
#include <evaluation/ErrorReport.h>
#include <git_sha1.h>

logger::LogChannel pytedlog("pytedlog", "[Ted] ");

class PyTed {

public:

	PyTed(const ErrorReport::Parameters& parameters = ErrorReport::Parameters()) :
		_parameters(parameters),
		_numThreads(0) {

		LOG_DEBUG(pytedlog) << "[Ted] constructed" << std::endl;
		initialize();
	}

	void setNumThreads(int numThreads) { _numThreads = numThreads; }

	boost::python::dict createReport(PyObject* gt, PyObject* rec, PyObject* voxel_size) {

		util::ProgramOptions::setOptionValue("numThreads", util::to_string(_numThreads));

		boost::python::dict summary;

		pipeline::Value<ImageStack> groundTruth = imageStackFromArray(gt, voxel_size);
		pipeline::Value<ImageStack> reconstruction = imageStackFromArray(rec, voxel_size);

		pipeline::Process<ErrorReport> report(_parameters);
		report->setInput("reconstruction", reconstruction);
		report->setInput("ground truth", groundTruth);

		if (_parameters.reportVoi) {

			pipeline::Value<VariationOfInformationErrors> voiErrors = report->getOutput("voi errors");
			summary["voi_split"] = voiErrors->getSplitEntropy();
			summary["voi_merge"] = voiErrors->getMergeEntropy();
		}

		if (_parameters.reportRand) {

			pipeline::Value<RandIndexErrors> randErrors = report->getOutput("rand errors");
			summary["rand_index"] = randErrors->getRandIndex();
			summary["rand_precision"] = randErrors->getPrecision();
			summary["rand_recall"] = randErrors->getRecall();
			summary["adapted_rand_error"] = randErrors->getAdaptedRandError();
		}

		if (_parameters.reportTed) {

			pipeline::Value<TolerantEditDistanceErrors> tedErrors = report->getOutput("ted errors");
			summary["ted_split"] = tedErrors->getNumSplits();
			summary["ted_merge"] = tedErrors->getNumMerges();
			summary["ted_fp"] = tedErrors->getNumFalsePositives();
			summary["ted_fn"] = tedErrors->getNumFalseNegatives();
			summary["ted_inference_time"] = tedErrors->getInferenceTime();
			summary["ted_num_variables"] = tedErrors->getNumVariables();
		}

		summary["ted_version"] = std::string(__git_sha1);

		return summary;
	}

private:

	pipeline::Value<ImageStack> imageStackFromArray(PyObject* a, PyObject* voxel_size) {

		// create (expect?) a C contiguous uint32 array
		PyArray_Descr* descr = PyArray_DescrFromType(NPY_UINT32);
		PyArrayObject* array = (PyArrayObject*)(PyArray_FromAny(a, descr, 2, 3, 0, NULL));

        PyArray_Descr* vs_descr = PyArray_DescrFromType(NPY_FLOAT64);
        PyArrayObject* vs = (PyArrayObject*)(PyArray_FromAny(voxel_size, vs_descr, 1, 1, 0, NULL));

        if (vs == NULL)
            UTIL_THROW_EXCEPTION(
					UsageError,
					"only voxel size arrays of dimension 1, with datatype np.float64 are supported");

		if (array == NULL)
			UTIL_THROW_EXCEPTION(
					UsageError,
					"only arrays of dimension 2 or 3, with datatype np.uint32 are supported");

		LOG_DEBUG(pytedlog) << "converted to array" << std::endl;

		int dims = PyArray_NDIM(array);
		size_t width, height, depth;

		if (dims == 2) {

			depth = 1;
			height = PyArray_DIM(array, 0);
			width = PyArray_DIM(array, 1);

		} else {

			depth =  PyArray_DIM(array, 0);
			height = PyArray_DIM(array, 1);
			width = PyArray_DIM(array, 2);
		}

		LOG_DEBUG(pytedlog) << "copying data..." << std::endl;

		pipeline::Value<ImageStack> stack;

        double res_x = *static_cast<double*>(PyArray_GETPTR1(vs, 0));
        double res_y = *static_cast<double*>(PyArray_GETPTR1(vs, 1));
        double res_z = *static_cast<double*>(PyArray_GETPTR1(vs, 2));

        stack->DiscreteVolume::setResolution(res_x, res_y, res_z);

		for (size_t z = 0; z < depth; z++) {

			boost::shared_ptr<Image> image = boost::make_shared<Image>(width, height);
			for (size_t y = 0; y < height; y++)
				for (size_t x = 0; x < width; x++) {

					uint32_t value;
					if (dims == 2)
						value = *static_cast<uint32_t*>(PyArray_GETPTR2(array, y, x));
					else
						value = *static_cast<uint32_t*>(PyArray_GETPTR3(array, z, y, x));

					(*image)(x,y) = value;
				}

			stack->add(image);
		}

		LOG_DEBUG(pytedlog) << "done" << std::endl;

		return stack;
	}

	void initialize() {

		// import_array is a macro expanding to returning different types across 
		// different versions of the NumPy API. This lambda hack works around 
		// that.
		auto a = []{ import_array(); };
		a();
	}

	ErrorReport::Parameters _parameters;

	int _numThreads;
};
