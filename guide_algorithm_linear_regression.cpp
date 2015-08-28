//
//  guide_linear_regression.cpp
//  PHD2 Guiding
//
//  Created by Edgar Klenske.
//  Copyright 2015, Max Planck Society.

/*
*  This source code is distributed under the following "BSD" license
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions are met:
*    Redistributions of source code must retain the above copyright notice,
*     this list of conditions and the following disclaimer.
*    Redistributions in binary form must reproduce the above copyright notice,
*     this list of conditions and the following disclaimer in the
*     documentation and/or other materials provided with the distribution.
*    Neither the name of Bret McKee, Dad Dog Development, nor the names of its
*     Craig Stark, Stark Labs nor the names of its
*     contributors may be used to endorse or promote products derived from
*     this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
*  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
*  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
*  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
*  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
*  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
*  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
*  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
*  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
*  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*/

#include "phd.h"

#include "guide_algorithm_linear_regression.h"
#include <wx/stopwatch.h>

#include "math_tools.h"

class GuideLinearRegression::GuideLinearRegressionDialogPane : public ConfigDialogPane
{
    GuideLinearRegression *m_pGuideAlgorithm;
    wxSpinCtrlDouble *m_pControlGain;
    wxSpinCtrl       *m_pNbMeasurementMin;

public:
    GuideLinearRegressionDialogPane(wxWindow *pParent, GuideLinearRegression *pGuideAlgorithm)
      : ConfigDialogPane(_("Linear Regression Guide Algorithm"),pParent)
    {
        m_pGuideAlgorithm = pGuideAlgorithm;

        int width = StringWidth(_T("00000.00"));
        m_pControlGain = new wxSpinCtrlDouble(pParent, wxID_ANY, wxEmptyString,
                                              wxDefaultPosition,wxSize(width+30, -1),
                                              wxSP_ARROW_KEYS, 0.0, 1.0, 0.8, 0.05);
        m_pControlGain->SetDigits(2);

        // nb elements before starting the inference
        m_pNbMeasurementMin = new wxSpinCtrl(pParent, wxID_ANY, wxEmptyString,
                                             wxDefaultPosition,wxSize(width+30, -1),
                                             wxSP_ARROW_KEYS, 0, 100, 25);

        DoAdd(_("Control Gain"), m_pControlGain,
              _("The control gain defines how aggressive the controller is. It is the amount of pointing error that is "
                "fed back to the system. Default = 0.8"));

        DoAdd(_("Min data points (inference)"), m_pNbMeasurementMin,
              _("Minimal number of measurements to start using the Linear Regression. If there are too little data points, "
                "the result might be poor. Default = 25"));

    }

    virtual ~GuideLinearRegressionDialogPane(void)
    {
      // no need to destroy the widgets, this is done by the parent...
    }

    /* Fill the GUI with the parameters that are currently chosen in the
      * guiding algorithm
      */
    virtual void LoadValues(void)
    {
      m_pControlGain->SetValue(m_pGuideAlgorithm->GetControlGain());
      m_pNbMeasurementMin->SetValue(m_pGuideAlgorithm->GetNbMeasurementsMin());
    }

    // Set the parameters chosen in the GUI in the actual guiding algorithm
    virtual void UnloadValues(void)
    {
      m_pGuideAlgorithm->SetControlGain(m_pControlGain->GetValue());
      m_pGuideAlgorithm->SetNbElementForInference(m_pNbMeasurementMin->GetValue());
    }

};


struct lr_guiding_circular_datapoints
{
  double timestamp;
  double measurement;
  double modified_measurement;
  double control;
};


// parameters of the LR guiding algorithm
struct GuideLinearRegression::lr_guide_parameters
{
    typedef lr_guiding_circular_datapoints data_points;
    circular_buffer<data_points> circular_buffer_parameters;

    wxStopWatch timer_;
    double control_signal_;
    double control_gain_;
    double last_timestamp_;
    double filtered_signal_;
    double mixing_parameter_;

    int min_nb_element_for_inference;

    lr_guide_parameters() :
      circular_buffer_parameters(200),
      timer_(),
      control_signal_(0.0),
      last_timestamp_(0.0),
      filtered_signal_(0.0),
      min_nb_element_for_inference(0)
    {

    }

    data_points& get_last_point()
    {
      return circular_buffer_parameters[circular_buffer_parameters.size() - 1];
    }

    data_points& get_second_last_point()
    {
      return circular_buffer_parameters[circular_buffer_parameters.size() - 2];
    }

    size_t get_number_of_measurements() const
    {
      return circular_buffer_parameters.size();
    }

    void add_one_point()
    {
      circular_buffer_parameters.push_front(data_points());
    }

    void clear()
    {
      circular_buffer_parameters.clear();
    }

};



static const double DefaultControlGain = 1.0;           // control gain
static const int    DefaultNbMinPointsForInference = 25; // minimal number of points for doing the inference

GuideLinearRegression::GuideLinearRegression(Mount *pMount, GuideAxis axis)
    : GuideAlgorithm(pMount, axis),
      parameters(0)
{
    parameters = new lr_guide_parameters();
    wxString configPath = GetConfigPath();

    double control_gain = pConfig->Profile.GetDouble(configPath + "/lr_controlGain", DefaultControlGain);
    SetControlGain(control_gain);

    int nb_element_for_inference = pConfig->Profile.GetInt(configPath + "/lr_nbminelementforinference", DefaultNbMinPointsForInference);
    SetNbElementForInference(nb_element_for_inference);

    reset();
}

GuideLinearRegression::~GuideLinearRegression(void)
{
    delete parameters;
}


ConfigDialogPane *GuideLinearRegression::GetConfigDialogPane(wxWindow *pParent)
{
    return new GuideLinearRegressionDialogPane(pParent, this);
}



bool GuideLinearRegression::SetControlGain(double control_gain)
{
    bool error = false;

    try
    {
        if (control_gain < 0 || control_gain > 1)
        {
            throw ERROR_INFO("invalid controlGain");
        }

        parameters->control_gain_ = control_gain;
    }
    catch (wxString Msg)
    {
        POSSIBLY_UNUSED(Msg);
        error = true;
        parameters->control_gain_ = DefaultControlGain;
    }

    pConfig->Profile.SetDouble(GetConfigPath() + "/lr_controlGain", parameters->control_gain_);

    return error;
}

bool GuideLinearRegression::SetNbElementForInference(int nb_elements)
{
    bool error = false;

    try
    {
        if (nb_elements < 0)
        {
            throw ERROR_INFO("invalid number of elements");
        }

        parameters->min_nb_element_for_inference = nb_elements;
    }
    catch (wxString Msg)
    {
        POSSIBLY_UNUSED(Msg);
        error = true;
        parameters->min_nb_element_for_inference = DefaultNbMinPointsForInference;
    }

    pConfig->Profile.SetInt(GetConfigPath() + "/lr_nbminelementforinference", parameters->min_nb_element_for_inference);

    return error;
}

double GuideLinearRegression::GetControlGain() const
{
    return parameters->control_gain_;
}

int GuideLinearRegression::GetNbMeasurementsMin() const
{
    return parameters->min_nb_element_for_inference;
}

wxString GuideLinearRegression::GetSettingsSummary()
{
    static const char* format =
      "Control Gain = %.3f\n";

    return wxString::Format(
      format,
      GetControlGain();
}


GUIDE_ALGORITHM GuideLinearRegression::Algorithm(void)
{
    return GUIDE_ALGORITHM_LINEAR_REGRESSION;
}

void GuideLinearRegression::HandleTimestamps()
{
    if (parameters->get_number_of_measurements() == 0)
    {
        parameters->timer_.Start();
    }
    double time_now = parameters->timer_.Time();
    double delta_measurement_time_ms = time_now - parameters->last_timestamp_;
    parameters->last_timestamp_ = time_now;
    parameters->get_last_point().timestamp = (parameters->last_timestamp_ - delta_measurement_time_ms / 2) / 1000;
}

// adds a new measurement to the circular buffer that holds the data.
void GuideLinearRegression::HandleMeasurements(double input)
{
    parameters->get_last_point().measurement = input;
}

void GuideLinearRegression::HandleControls(double control_input)
{
    if(parameters->get_number_of_measurements() <= 1)
    {
        parameters->get_last_point().control = control_input;
    }
    else
    {
        parameters->get_last_point().control
}

void GuideLinearRegression::HandleModifiedMeasurements(double input)
{
    if(parameters->get_number_of_measurements() <= 1)
    {
        // At step one, ideal would have been to know this error in advance and correct it.
        parameters->get_last_point().modified_measurement = input;
    }
    else
    {
        /* Later, we already did some control action. Therefore, we have to sum over the past control signal.
         *
         * The ideal control signal is what would have been needed to keep a zero error at zero.
         */
        parameters->get_last_point().modified_measurement = input // the current displacement should have been corrected for
            + parameters->get_second_last_point().control // we already did control in the last step, we have to add this
            - parameters->get_second_last_point().measurement // if we previously weren't at zero, we have to subtract the offset
            + parameters->get_second_last_point().modified_measurement; // this is the integration step
    }
}

double GuideLinearRegression::result(double input)
{
    parameters->add_one_point();
    HandleMeasurements(input);
    HandleTimestamps();

    int delta_controller_time_ms = pFrame->RequestedExposureDuration();

    parameters->control_signal_ = parameters->control_gain_*input; // add the measured part of the controller
    double prediction_ = 0.0; // this will hold the prediction part later

    // initialize the different vectors needed for the linear regression
    Eigen::VectorXd timestamps(parameters->get_number_of_measurements());
    Eigen::VectorXd measurements(parameters->get_number_of_measurements());

    // check if we are allowed to use the LR
    if (parameters->min_nb_element_for_inference > 0 &&
        parameters->get_number_of_measurements() > parameters->min_nb_element_for_inference)
    {
        // transfer the data from the circular buffer to the Eigen::Vectors
        for(size_t i = 0; i < parameters->get_number_of_measurements(); i++)
        {
            timestamps(i) = parameters->circular_buffer_parameters[i].timestamp;
            // we combine both the integrated control and the residual error here, since it reflects the overall error
            if (parameters->get_number_of_measurements() == 1)
            {
                measurements(i) = circular_buffer_parameters[i].measurement; // for the first data point, there is no control value
            }
            else
            {
                measurements(i) = parameters->circular_buffer_parameters[i-1].control + circular_buffer_parameters[i].measurement;
            }
        }

        // linear least squares regression for offset and drift
        Eigen::MatrixXd feature_matrix(2, parameters->get_number_of_measurements()-1);
        feature_matrix.row(0) = timestamps.array().pow(0);
        feature_matrix.row(1) = timestamps.array(); // .pow(1) would be kinda useless

        // this is the inference for linear regression
        Eigen::VectorXd weights = (feature_matrix*feature_matrix.transpose()
            + 1e-3*Eigen::Matrix<double, 2, 2>::Identity()).ldlt().solve(feature_matrix*mod_measurements);

        // the prediction is only linear drift here
        prediction_ = (delta_controller_time_ms / 1000)*weights(1);
        parameters->control_signal_ += prediction_; // add in the prediction
    }

    HandleControls(parameters->control_signal_);

    return parameters->control_signal_;
}

double GuideLinearRegression::deduceResult(void)
{
    int delta_controller_time_ms = pFrame->RequestedExposureDuration();

    double prediction_ = 0.0; // this will hold the prediction part later

    // initialize the different vectors needed for the linear regression
    Eigen::VectorXd timestamps(parameters->get_number_of_measurements());
    Eigen::VectorXd measurements(parameters->get_number_of_measurements());

    // check if we are allowed to use the LR
    if (parameters->min_nb_element_for_inference > 0 &&
        parameters->get_number_of_measurements() > parameters->min_nb_element_for_inference)
    {
        // transfer the data from the circular buffer to the Eigen::Vectors
        for(size_t i = 0; i < parameters->get_number_of_measurements(); i++)
        {
            timestamps(i) = parameters->circular_buffer_parameters[i].timestamp;
            // we combine both the integrated control and the residual error here, since it reflects the overall error
            if (parameters->get_number_of_measurements() == 1)
            {
                measurements(i) = circular_buffer_parameters[i].measurement; // for the first data point, there is no control value
            }
            else
            {
                measurements(i) = parameters->circular_buffer_parameters[i-1].control + circular_buffer_parameters[i].measurement;
            }
        }

        // linear least squares regression for offset and drift
        Eigen::MatrixXd feature_matrix(2, parameters->get_number_of_measurements()-1);
        feature_matrix.row(0) = timestamps.array().pow(0);
        feature_matrix.row(1) = timestamps.array(); // .pow(1) would be kinda useless

        // this is the inference for linear regression
        Eigen::VectorXd weights = (feature_matrix*feature_matrix.transpose()
        + 1e-3*Eigen::Matrix<double, 2, 2>::Identity()).ldlt().solve(feature_matrix*mod_measurements);

        // the prediction is only linear drift here
        prediction_ = (delta_controller_time_ms / 1000)*weights(1);
        parameters->control_signal_ = prediction_; // add in the prediction
    }

    HandleControls(parameters->control_signal_);

    return parameters->control_signal_;
}

void GuideLinearRegression::reset()
{
    parameters->clear();
    return;
}